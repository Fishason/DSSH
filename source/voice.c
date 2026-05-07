#include "voice.h"
#include "ssh_client.h"

#include <3ds.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Tunables ────────────────────────────────────────────────────────
 *
 * MIC_BUFFER_SIZE: 1 MB at 16 kHz PCM16 = ~32 seconds of audio.  Must
 * be aligned to 0x1000 (libctru requirement).  When loop=false the mic
 * automatically stops sampling once the buffer fills, which is our
 * hardware-side ceiling.  Bumped from 256 KB / 8 s in M12 — voice IME
 * users never need that much, but AI Q&A users sometimes ask multi-
 * sentence questions, and 1 MB is free on a 64 MB heap.
 *
 * MAX_RECORD_FRAMES: software cap one frame below the hardware ceiling
 * so the user's "stop now" press always sees a sane buffer state.
 *
 * ERROR_DECAY_FRAMES: how many 60 fps frames the red "ERR" badge
 * lingers before auto-clearing back to IDLE.
 *
 * MIN_PCM_BYTES: anything smaller is treated as a fumbled press.
 * 4 KB ≈ 125 ms at 16 kHz PCM16 — too short to be meaningful speech.
 *
 * AI_HISTORY_MAX: number of (question, answer) pairs to keep across
 * follow-up turns.  Each pair is up to AI_Q_MAX + AI_A_MAX bytes; 5
 * turns × ~4.5 KB ≈ 22 KB total — small alongside the 1 MB mic buf.
 * ────────────────────────────────────────────────────────────────── */
#define MIC_BUFFER_SIZE    (1024 * 1024)
#define MIC_BUFFER_ALIGN   0x1000
#define MAX_RECORD_FRAMES  (60 * 30)
#define ERROR_DECAY_FRAMES 120
#define MIN_PCM_BYTES      4096
#define WRITE_CHUNK_BYTES  4096
#define REPLY_BUF_SIZE     8192       /* AI JSON envelopes are bigger */

#define AI_HISTORY_MAX     5
#define AI_Q_MAX           512
#define AI_A_MAX           4096

/* Use ~ rather than bare "dssh-whisper-shim" because libssh2_channel_exec
 * runs the command in a non-interactive shell that does NOT source the
 * user's .bashrc, so ~/.local/bin is NOT on PATH at exec time.  The
 * shell still expands ~ to $HOME though (that's parameter expansion,
 * not PATH lookup), so the absolute path resolves correctly. */
#define WHISPER_SHIM_CMD       "~/.local/bin/dssh-whisper-shim"
#define WHISPER_SHIM_ASK_CMD   "~/.local/bin/dssh-whisper-shim --ask"

struct voice_t {
    voice_state_t state;
    int           state_frame;        /* per-tick counter for animation/timeouts */

    int           ai_mode;            /* 1 if current cycle is AI-ask */

    uint8_t      *mic_buf;
    uint32_t      mic_buf_size;
    int           mic_active;
    uint32_t      pcm_len;            /* bytes captured at stop */

    /* Upload payload — for non-AI mode this just points into mic_buf;
     * for AI mode we allocate `xfer_owned` containing the framed
     * (4-byte BE PCM length + PCM + history JSON) blob. */
    const uint8_t *xfer_buf;
    uint32_t      xfer_len;
    uint32_t      xfer_pos;
    uint8_t      *xfer_owned;         /* if non-NULL, free in release_aux */

    ssh_aux_channel_t *aux;
    int           xfer_phase;         /* 0=writing, 1=eof-pending, 2=reading */
    char          reply_buf[REPLY_BUF_SIZE];
    int           reply_len;

    /* AI-ask state.  ai_question + ai_answer are filled from the JSON
     * response body after VOICE_TRANSCRIBING completes; ai_history is
     * appended to on close_keep, cleared on close_clear. */
    char          ai_question[AI_Q_MAX];
    char          ai_answer[AI_A_MAX];
    char          ai_hist_q[AI_HISTORY_MAX][AI_Q_MAX];
    char          ai_hist_a[AI_HISTORY_MAX][AI_A_MAX];
    int           ai_history_n;

    char          err_msg[32];        /* last error reason (for debug) */
};

/* ── Helpers ────────────────────────────────────────────────────────── */

static void enter_idle(voice_t *v) {
    v->state = VOICE_IDLE;
    v->state_frame = 0;
    v->ai_mode = 0;
}

static void enter_error(voice_t *v, const char *msg) {
    v->state = VOICE_ERROR;
    v->state_frame = 0;
    if (msg) snprintf(v->err_msg, sizeof(v->err_msg), "%s", msg);
}

static void release_aux(voice_t *v) {
    if (v->aux) {
        ssh_aux_close(v->aux);
        v->aux = NULL;
    }
    if (v->xfer_owned) {
        free(v->xfer_owned);
        v->xfer_owned = NULL;
    }
    v->xfer_buf = NULL;
    v->xfer_len = 0;
    v->xfer_pos = 0;
}

static int start_recording(voice_t *v) {
    Result rc = micInit(v->mic_buf, v->mic_buf_size);
    if (R_FAILED(rc)) { enter_error(v, "mic init failed"); return -1; }

    /* loop=false → mic stops sampling automatically when the buffer
     * fills.  16 kHz signed PCM16 mono.  -4 reserved bytes per libctru
     * convention so the offset cursor never wraps past the buffer. */
    rc = MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED,
                            MICU_SAMPLE_RATE_16360,
                            0, v->mic_buf_size - 4, false);
    if (R_FAILED(rc)) {
        micExit();
        enter_error(v, "mic start failed");
        return -1;
    }

    v->mic_active   = 1;
    v->pcm_len      = 0;
    v->state        = VOICE_RECORDING;
    v->state_frame  = 0;
    return 0;
}

static void stop_mic_capture(voice_t *v) {
    if (!v->mic_active) return;
    v->pcm_len = micGetLastSampleOffset();
    if (v->pcm_len > v->mic_buf_size) v->pcm_len = v->mic_buf_size;
    MICU_StopSampling();
    micExit();
    v->mic_active = 0;
}

/* ── JSON helpers (minimal escape — only what we emit/consume) ────── */

/* Append `src` to `dst[*pos..cap-1]` with JSON-string escaping for the
 * characters we plausibly see in transcribed Mandarin / shell answers
 * (\, ", control chars).  Returns 0 on success, -1 if buffer ran out.
 * We do NOT escape >=0x80 — UTF-8 multibyte sequences are valid in
 * JSON strings as-is. */
static int json_append_string(char *dst, int cap, int *pos, const char *src) {
    int p = *pos;
    if (p >= cap - 1) return -1;
    dst[p++] = '"';
    for (const unsigned char *s = (const unsigned char *)src; *s; s++) {
        unsigned char c = *s;
        const char *esc = NULL;
        char esc_buf[8];
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\b': esc = "\\b";  break;
            case '\f': esc = "\\f";  break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default:
                if (c < 0x20) {
                    snprintf(esc_buf, sizeof(esc_buf), "\\u%04x", c);
                    esc = esc_buf;
                }
        }
        if (esc) {
            int el = (int)strlen(esc);
            if (p + el >= cap - 1) return -1;
            memcpy(dst + p, esc, (size_t)el);
            p += el;
        } else {
            if (p >= cap - 2) return -1;
            dst[p++] = (char)c;
        }
    }
    if (p >= cap - 1) return -1;
    dst[p++] = '"';
    *pos = p;
    return 0;
}

/* Build the history JSON: [["Q1","A1"],["Q2","A2"], ...].  Returns the
 * number of bytes written into `dst[0..cap-1]`; caller must NUL-terminate
 * if it wants a C string (we don't, since we ship raw bytes over the
 * channel).  Returns -1 on overflow. */
static int build_history_json(const voice_t *v, char *dst, int cap) {
    int p = 0;
    if (cap < 3) return -1;
    dst[p++] = '[';
    for (int i = 0; i < v->ai_history_n; i++) {
        if (i > 0) {
            if (p >= cap - 1) return -1;
            dst[p++] = ',';
        }
        if (p >= cap - 1) return -1;
        dst[p++] = '[';
        if (json_append_string(dst, cap, &p, v->ai_hist_q[i]) < 0) return -1;
        if (p >= cap - 1) return -1;
        dst[p++] = ',';
        if (json_append_string(dst, cap, &p, v->ai_hist_a[i]) < 0) return -1;
        if (p >= cap - 1) return -1;
        dst[p++] = ']';
    }
    if (p >= cap - 1) return -1;
    dst[p++] = ']';
    return p;
}

/* Decode a JSON string starting at *p (which must point to the opening
 * `"`); copy the unescaped UTF-8 contents into `out[0..out_cap-1]` and
 * NUL-terminate.  Advances *p past the closing `"`.  Returns 0 on
 * success, -1 on malformed input.  Implements only the escapes we
 * actually emit on the server side: \" \\ \/ \b \f \n \r \t \uXXXX
 * (the rest fall through as literal). */
static int json_extract_string(const char **p, const char *end,
                               char *out, int out_cap) {
    if (*p >= end || **p != '"') return -1;
    (*p)++;
    int o = 0;
    while (*p < end && **p != '"') {
        unsigned char c = (unsigned char)**p;
        if (c == '\\') {
            (*p)++;
            if (*p >= end) return -1;
            unsigned char esc = (unsigned char)**p;
            (*p)++;
            char emit[4]; int emit_len = 1;
            switch (esc) {
                case '"':  emit[0] = '"';  break;
                case '\\': emit[0] = '\\'; break;
                case '/':  emit[0] = '/';  break;
                case 'b':  emit[0] = '\b'; break;
                case 'f':  emit[0] = '\f'; break;
                case 'n':  emit[0] = '\n'; break;
                case 'r':  emit[0] = '\r'; break;
                case 't':  emit[0] = '\t'; break;
                case 'u': {
                    if (*p + 4 > end) return -1;
                    unsigned cp = 0;
                    for (int i = 0; i < 4; i++) {
                        char hc = *(*p)++;
                        cp <<= 4;
                        if (hc >= '0' && hc <= '9') cp |= (unsigned)(hc - '0');
                        else if (hc >= 'a' && hc <= 'f') cp |= (unsigned)(hc - 'a' + 10);
                        else if (hc >= 'A' && hc <= 'F') cp |= (unsigned)(hc - 'A' + 10);
                        else return -1;
                    }
                    /* Encode codepoint as UTF-8.  The server prompt only
                     * really emits low-ASCII via \u, so this branch is
                     * rare, but stay correct for surrogate pairs etc. */
                    if (cp < 0x80) {
                        emit[0] = (char)cp; emit_len = 1;
                    } else if (cp < 0x800) {
                        emit[0] = (char)(0xC0 | (cp >> 6));
                        emit[1] = (char)(0x80 | (cp & 0x3F));
                        emit_len = 2;
                    } else {
                        emit[0] = (char)(0xE0 | (cp >> 12));
                        emit[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        emit[2] = (char)(0x80 | (cp & 0x3F));
                        emit_len = 3;
                    }
                    break;
                }
                default:
                    /* Unknown escape — pass through literally. */
                    emit[0] = (char)esc; emit_len = 1;
            }
            for (int i = 0; i < emit_len; i++) {
                if (o < out_cap - 1) out[o++] = emit[i];
            }
        } else {
            if (o < out_cap - 1) out[o++] = (char)c;
            (*p)++;
        }
    }
    if (*p >= end || **p != '"') return -1;
    (*p)++;
    out[o] = 0;
    return 0;
}

/* Find a top-level "key": value pair in flat JSON `body[0..body_len)`
 * and extract the string value into `out`.  Tolerant of whitespace.
 * Returns 0 on success, -1 if not found / malformed. */
static int json_find_string_field(const char *body, int body_len,
                                  const char *key,
                                  char *out, int out_cap) {
    /* Build needle: "key" */
    char needle[64];
    int kl = (int)strlen(key);
    if (kl + 3 >= (int)sizeof(needle)) return -1;
    needle[0] = '"';
    memcpy(needle + 1, key, (size_t)kl);
    needle[kl + 1] = '"';
    needle[kl + 2] = 0;
    const char *end = body + body_len;
    const char *p = body;
    while (p < end) {
        const char *hit = strstr(p, needle);
        if (!hit || hit >= end) return -1;
        const char *q = hit + kl + 2;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\n')) q++;
        if (q >= end || *q != ':') { p = hit + 1; continue; }
        q++;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\n')) q++;
        if (q >= end || *q != '"') { p = hit + 1; continue; }
        return json_extract_string(&q, end, out, out_cap);
    }
    return -1;
}

/* RECORDING → TRANSCRIBING.  Called both on the user's "stop" tap and
 * on the hardware/software duration cap. */
static void begin_transcribe(voice_t *v, ssh_client_t *ssh) {
    stop_mic_capture(v);

    if (v->pcm_len < MIN_PCM_BYTES) {
        /* Fumbled press — pretend nothing happened. */
        enter_idle(v);
        return;
    }

    /* Build the upload payload depending on AI mode.
     *   non-AI: just send raw PCM (xfer_buf = mic_buf).
     *   AI:     prefix with 4-byte BE PCM length, then PCM, then
     *           history JSON.  Lives in xfer_owned.  */
    if (v->ai_mode) {
        /* History JSON max = 5 * (~5 KB Q+A overhead with escaping) ≈ 30 KB.
         * Allocate generously: 4 + pcm_len + 64 KB for JSON. */
        int hist_cap = 64 * 1024;
        size_t total = 4 + v->pcm_len + (size_t)hist_cap;
        v->xfer_owned = (uint8_t *)malloc(total);
        if (!v->xfer_owned) { enter_error(v, "ai oom"); return; }

        v->xfer_owned[0] = (uint8_t)((v->pcm_len >> 24) & 0xff);
        v->xfer_owned[1] = (uint8_t)((v->pcm_len >> 16) & 0xff);
        v->xfer_owned[2] = (uint8_t)((v->pcm_len >>  8) & 0xff);
        v->xfer_owned[3] = (uint8_t)((v->pcm_len      ) & 0xff);
        memcpy(v->xfer_owned + 4, v->mic_buf, v->pcm_len);

        int hist_len = build_history_json(v, (char *)(v->xfer_owned + 4 + v->pcm_len),
                                          hist_cap);
        if (hist_len < 0) {
            free(v->xfer_owned); v->xfer_owned = NULL;
            enter_error(v, "ai hist json");
            return;
        }
        v->xfer_buf = v->xfer_owned;
        v->xfer_len = (uint32_t)(4 + v->pcm_len + (uint32_t)hist_len);
    } else {
        v->xfer_buf = v->mic_buf;
        v->xfer_len = v->pcm_len;
    }

    char err[64] = {0};
    const char *cmd = v->ai_mode ? WHISPER_SHIM_ASK_CMD : WHISPER_SHIM_CMD;
    v->aux = ssh_aux_exec(ssh, cmd, err, sizeof(err));
    if (!v->aux) {
        if (v->xfer_owned) { free(v->xfer_owned); v->xfer_owned = NULL; }
        v->xfer_buf = NULL;
        enter_error(v, err[0] ? err : "exec failed");
        return;
    }

    v->state         = VOICE_TRANSCRIBING;
    v->state_frame   = 0;
    v->xfer_phase    = 0;
    v->xfer_pos      = 0;
    v->reply_len     = 0;
}

/* On VOICE_TRANSCRIBING completion in AI mode: parse JSON body, fill
 * ai_question + ai_answer, transition into the modal-display state.
 *
 * Diagnostic fallback: if parsing fails OR returns an empty field, we
 * dump the raw reply_buf bytes into ai_answer so the modal at least
 * shows what arrived from the server (useful when chasing protocol
 * bugs).  Prefix the dump with a tag so it's visually distinct from
 * legitimate AI answers. */
static void finish_ai_transcribe(voice_t *v) {
    if (v->reply_len <= 0) {
        enter_error(v, "ai empty reply");
        return;
    }

    int q_rc = json_find_string_field(v->reply_buf, v->reply_len,
                                      "question",
                                      v->ai_question,
                                      sizeof(v->ai_question));
    int a_rc = json_find_string_field(v->reply_buf, v->reply_len,
                                      "answer",
                                      v->ai_answer,
                                      sizeof(v->ai_answer));
    if (q_rc < 0) v->ai_question[0] = 0;
    if (a_rc < 0) v->ai_answer[0]   = 0;

    /* Diagnostic dump — shows up in the modal answer area whenever
     * parsing produced an empty payload.  Lets the user see what the
     * 3DS actually received vs. what the server logged it sent. */
    if (v->ai_answer[0] == 0) {
        const char prefix[] = "[parse-fail dump rc q=";
        int o = 0;
        int avail = (int)sizeof(v->ai_answer) - 1;
        int pl = (int)sizeof(prefix) - 1;
        if (o + pl < avail) { memcpy(v->ai_answer + o, prefix, pl); o += pl; }
        v->ai_answer[o++] = (char)('0' + (q_rc < 0 ? 1 : 0));
        if (o + 4 < avail) { memcpy(v->ai_answer + o, " a=", 3); o += 3; }
        v->ai_answer[o++] = (char)('0' + (a_rc < 0 ? 1 : 0));
        if (o + 8 < avail) { memcpy(v->ai_answer + o, " len=", 5); o += 5; }
        /* Print reply_len as decimal. */
        char num[12];
        int nl = snprintf(num, sizeof(num), "%d", v->reply_len);
        if (o + nl < avail) { memcpy(v->ai_answer + o, num, (size_t)nl); o += nl; }
        if (o + 3 < avail) { memcpy(v->ai_answer + o, "]\n", 2); o += 2; }
        /* Then the raw reply_buf bytes, truncated to fit the buffer. */
        int n = v->reply_len;
        if (n > avail - o) n = avail - o;
        memcpy(v->ai_answer + o, v->reply_buf, (size_t)n);
        o += n;
        v->ai_answer[o] = 0;
    }

    v->state       = VOICE_AI_SHOWING;
    v->state_frame = 0;
    /* ai_mode stays 1 until close_clear/close_keep so softkb knows. */
}

/* ── Public API ─────────────────────────────────────────────────────── */

voice_t *voice_init(void) {
    voice_t *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->mic_buf = memalign(MIC_BUFFER_ALIGN, MIC_BUFFER_SIZE);
    if (!v->mic_buf) { free(v); return NULL; }
    v->mic_buf_size = MIC_BUFFER_SIZE;
    return v;
}

void voice_free(voice_t *v) {
    if (!v) return;
    if (v->mic_active) {
        MICU_StopSampling();
        micExit();
    }
    release_aux(v);
    if (v->mic_buf) free(v->mic_buf);
    free(v);
}

void voice_toggle(voice_t *v, ssh_client_t *ssh) {
    if (!v || !ssh) return;
    switch (v->state) {
        case VOICE_IDLE:
            v->ai_mode = 0;
            start_recording(v);
            break;
        case VOICE_RECORDING:
            begin_transcribe(v, ssh);
            break;
        case VOICE_TRANSCRIBING:
            /* User pressed START again → cancel an in-flight transcribe. */
            release_aux(v);
            enter_idle(v);
            break;
        case VOICE_AI_SHOWING:
            /* Plain START while modal up — let the modal stay; the
             * caller (main.c) routes start back to us only when modal
             * is closed.  Defensive: ignore. */
            break;
        case VOICE_ERROR:
            enter_idle(v);
            break;
    }
}

void voice_ai_toggle(voice_t *v, ssh_client_t *ssh) {
    if (!v || !ssh) return;
    switch (v->state) {
        case VOICE_IDLE:
            v->ai_mode = 1;
            start_recording(v);
            break;
        case VOICE_RECORDING:
            /* Already recording — finalize and ship to AI (whatever
             * START was originally pressed for, the user just promoted
             * to AI by the L modifier).  Keep ai_mode set if it was
             * already set; otherwise upgrade to AI mode now. */
            v->ai_mode = 1;
            begin_transcribe(v, ssh);
            break;
        case VOICE_TRANSCRIBING:
            release_aux(v);
            enter_idle(v);
            break;
        case VOICE_AI_SHOWING:
            /* Modal up — main.c shouldn't route here; defensive ignore. */
            break;
        case VOICE_ERROR:
            enter_idle(v);
            break;
    }
}

void voice_ai_close_keep(voice_t *v) {
    if (!v || v->state != VOICE_AI_SHOWING) return;
    /* Append (Q,A) to history; FIFO-evict if at capacity. */
    if (v->ai_history_n >= AI_HISTORY_MAX) {
        for (int i = 1; i < AI_HISTORY_MAX; i++) {
            memcpy(v->ai_hist_q[i - 1], v->ai_hist_q[i], AI_Q_MAX);
            memcpy(v->ai_hist_a[i - 1], v->ai_hist_a[i], AI_A_MAX);
        }
        v->ai_history_n = AI_HISTORY_MAX - 1;
    }
    int slot = v->ai_history_n;
    snprintf(v->ai_hist_q[slot], AI_Q_MAX, "%s", v->ai_question);
    snprintf(v->ai_hist_a[slot], AI_A_MAX, "%s", v->ai_answer);
    v->ai_history_n++;
    enter_idle(v);
}

void voice_ai_close_clear(voice_t *v) {
    if (!v || v->state != VOICE_AI_SHOWING) return;
    v->ai_history_n = 0;
    enter_idle(v);
}

const char *voice_ai_question(const voice_t *v) {
    return v ? v->ai_question : "";
}
const char *voice_ai_answer(const voice_t *v) {
    return v ? v->ai_answer : "";
}
int voice_ai_history_count(const voice_t *v) {
    return v ? v->ai_history_n : 0;
}
int voice_in_ai_cycle(const voice_t *v) {
    return v ? v->ai_mode : 0;
}

void voice_tick(voice_t *v, ssh_client_t *ssh) {
    if (!v) return;
    v->state_frame++;

    switch (v->state) {
        case VOICE_IDLE:
            break;

        case VOICE_RECORDING:
            /* Auto-stop one frame before the hardware buffer fills so the
             * tick has time to transition cleanly. */
            if (v->state_frame >= MAX_RECORD_FRAMES) {
                begin_transcribe(v, ssh);
            }
            break;

        case VOICE_TRANSCRIBING: {
            if (!v->aux) { enter_error(v, "aux gone"); break; }

            if (v->xfer_phase == 0) {
                /* Stream audio + (AI) history → server. */
                uint32_t remaining = v->xfer_len - v->xfer_pos;
                if (remaining > 0) {
                    int chunk = (int)(remaining > WRITE_CHUNK_BYTES
                                      ? WRITE_CHUNK_BYTES
                                      : remaining);
                    int n = ssh_aux_write(v->aux,
                                          (const char *)(v->xfer_buf + v->xfer_pos),
                                          chunk);
                    if (n < 0) {
                        release_aux(v);
                        enter_error(v, "aux write");
                        break;
                    }
                    if (n > 0) v->xfer_pos += (uint32_t)n;
                }
                if (v->xfer_pos >= v->xfer_len) v->xfer_phase = 1;
            } else if (v->xfer_phase == 1) {
                /* Tell the shim "no more bytes coming". */
                int rc = ssh_aux_send_eof(v->aux);
                if (rc < 0) {
                    release_aux(v);
                    enter_error(v, "aux eof");
                    break;
                }
                if (rc == 0) v->xfer_phase = 2;
                /* rc == 1 → EAGAIN, retry next frame */
            } else /* phase 2 */ {
                /* Drain ALL bytes available this frame BEFORE checking
                 * EOF.  Earlier code did one read per frame, which
                 * could see ssh_aux_eof() return true while bytes were
                 * still buffered locally — causing finish_ai_transcribe
                 * to parse a half-arrived JSON and emit empty fields.
                 * Inner loop fixes that: read returns 0 only when there
                 * really is nothing left to consume right now. */
                int read_failed = 0;
                while (v->reply_len < (int)sizeof(v->reply_buf)) {
                    int n = ssh_aux_read(v->aux,
                                         v->reply_buf + v->reply_len,
                                         (int)sizeof(v->reply_buf) - v->reply_len);
                    if (n < 0) {
                        release_aux(v);
                        enter_error(v, "aux read");
                        read_failed = 1;
                        break;
                    }
                    if (n == 0) break;   /* EAGAIN — try again next frame */
                    v->reply_len += n;
                }
                if (read_failed) break;
                if (ssh_aux_eof(v->aux)) {
                    if (v->ai_mode) {
                        /* Parse JSON, transition to AI_SHOWING; release
                         * aux but keep ai_mode and history intact. */
                        if (v->aux) {
                            ssh_aux_close(v->aux);
                            v->aux = NULL;
                        }
                        if (v->xfer_owned) {
                            free(v->xfer_owned);
                            v->xfer_owned = NULL;
                        }
                        v->xfer_buf = NULL;
                        v->xfer_len = 0;
                        finish_ai_transcribe(v);
                    } else {
                        if (v->reply_len > 0) {
                            ssh_write(ssh, v->reply_buf, v->reply_len);
                        }
                        release_aux(v);
                        enter_idle(v);
                    }
                }
            }
            break;
        }

        case VOICE_AI_SHOWING:
            /* Idle until close_keep / close_clear.  state_frame keeps
             * incrementing so the modal renderer can drive its fade-in
             * animation off it. */
            break;

        case VOICE_ERROR:
            if (v->state_frame >= ERROR_DECAY_FRAMES) enter_idle(v);
            break;
    }
}

voice_state_t voice_state(const voice_t *v) { return v ? v->state : VOICE_IDLE; }
int           voice_state_frame(const voice_t *v) { return v ? v->state_frame : 0; }

/* 10-frame Braille spinner — the well-known cli-spinners "dots".
 * Each frame is " <braille> " (3-cell wide) so it fits the existing
 * 3-character status slot.  Cycles every 6 frames at 60 fps → ~10 Hz. */
static const char *spinner_frame(int frame) {
    static const char *frames[10] = {
        " \xe2\xa0\x8b ",  /* ⠋ */
        " \xe2\xa0\x99 ",  /* ⠙ */
        " \xe2\xa0\xb9 ",  /* ⠹ */
        " \xe2\xa0\xb8 ",  /* ⠸ */
        " \xe2\xa0\xbc ",  /* ⠼ */
        " \xe2\xa0\xb4 ",  /* ⠴ */
        " \xe2\xa0\xa6 ",  /* ⠦ */
        " \xe2\xa0\xa7 ",  /* ⠧ */
        " \xe2\xa0\x87 ",  /* ⠇ */
        " \xe2\xa0\x8f ",  /* ⠏ */
    };
    int i = (frame / 6) % 10;
    if (i < 0) i += 10;
    return frames[i];
}

const char *voice_status_label(const voice_t *v) {
    if (!v) return NULL;
    switch (v->state) {
        case VOICE_RECORDING:
            return v->ai_mode ? "AI?" : "REC";
        case VOICE_TRANSCRIBING:
            return spinner_frame(v->state_frame);
        case VOICE_ERROR:
            return "ERR";
        default:
            return NULL;
    }
}

uint32_t voice_status_bg(const voice_t *v) {
    if (!v) return 0;
    switch (v->state) {
        case VOICE_RECORDING: {
            /* Pulsing alpha — magenta tint when AI-cycle, red otherwise. */
            int t = v->state_frame % 60;
            int up = (t < 30) ? t : (60 - t);
            uint8_t alpha = 0x60 + (uint8_t)(up * 2);
            uint32_t base = v->ai_mode ? 0xbb9af700u : 0xf7768e00u;
            return base | alpha;
        }
        case VOICE_TRANSCRIBING:
            /* Magenta-ish when AI to differentiate from cyan-ish IME. */
            return v->ai_mode ? 0xbb9af780u : 0x7dcfc080u;
        case VOICE_ERROR:
            return 0xf7768eff;
        default:
            return 0;
    }
}

uint32_t voice_status_fg(const voice_t *v) {
    if (!v) return 0xc0caf5ff;
    switch (v->state) {
        case VOICE_RECORDING:
        case VOICE_ERROR:
        case VOICE_TRANSCRIBING:
            return 0x1a1b26ff;
        default:
            return 0xc0caf5ff;
    }
}
