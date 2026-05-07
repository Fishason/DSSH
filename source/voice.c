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
 * MIC_BUFFER_SIZE: 256 KB at 16 kHz PCM16 = ~8 seconds of audio.  Must
 * be aligned to 0x1000 (libctru requirement).  When loop=false the mic
 * automatically stops sampling once the buffer fills, which is our
 * hardware-side ceiling.
 *
 * MAX_RECORD_FRAMES: software cap one frame below the hardware ceiling
 * so the user's "stop now" press always sees a sane buffer state.
 *
 * ERROR_DECAY_FRAMES: how many 60 fps frames the red "ERR" badge
 * lingers before auto-clearing back to IDLE.
 *
 * MIN_PCM_BYTES: anything smaller is treated as a fumbled press.
 * 4 KB ≈ 125 ms at 16 kHz PCM16 — too short to be meaningful speech.
 * ────────────────────────────────────────────────────────────────── */
#define MIC_BUFFER_SIZE    (256 * 1024)
#define MIC_BUFFER_ALIGN   0x1000
#define MAX_RECORD_FRAMES  (60 * 7)
#define ERROR_DECAY_FRAMES 120
#define MIN_PCM_BYTES      4096
#define WRITE_CHUNK_BYTES  4096
#define REPLY_BUF_SIZE     4096

/* Use ~ rather than bare "dssh-whisper-shim" because libssh2_channel_exec
 * runs the command in a non-interactive shell that does NOT source the
 * user's .bashrc, so ~/.local/bin is NOT on PATH at exec time.  The
 * shell still expands ~ to $HOME though (that's parameter expansion,
 * not PATH lookup), so the absolute path resolves correctly. */
#define WHISPER_SHIM_CMD   "~/.local/bin/dssh-whisper-shim"

struct voice_t {
    voice_state_t state;
    int           state_frame;        /* per-tick counter for animation/timeouts */

    uint8_t      *mic_buf;
    uint32_t      mic_buf_size;
    int           mic_active;
    uint32_t      pcm_len;            /* bytes captured at stop */

    ssh_aux_channel_t *aux;
    int           xfer_phase;         /* 0=writing, 1=eof-pending, 2=reading */
    uint32_t      xfer_write_pos;
    char          reply_buf[REPLY_BUF_SIZE];
    int           reply_len;

    char          err_msg[32];        /* last error reason (for debug) */
};

/* ── Helpers ────────────────────────────────────────────────────────── */

static void enter_idle(voice_t *v) {
    v->state = VOICE_IDLE;
    v->state_frame = 0;
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

/* RECORDING → TRANSCRIBING.  Called both on the user's "stop" tap and
 * on the hardware/software duration cap. */
static void begin_transcribe(voice_t *v, ssh_client_t *ssh) {
    stop_mic_capture(v);

    if (v->pcm_len < MIN_PCM_BYTES) {
        /* Fumbled press — pretend nothing happened. */
        enter_idle(v);
        return;
    }

    char err[64] = {0};
    v->aux = ssh_aux_exec(ssh, WHISPER_SHIM_CMD, err, sizeof(err));
    if (!v->aux) {
        enter_error(v, err[0] ? err : "exec failed");
        return;
    }

    v->state         = VOICE_TRANSCRIBING;
    v->state_frame   = 0;
    v->xfer_phase    = 0;
    v->xfer_write_pos = 0;
    v->reply_len     = 0;
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
        case VOICE_ERROR:
            enter_idle(v);
            break;
    }
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
                /* Stream audio → server. */
                uint32_t remaining = v->pcm_len - v->xfer_write_pos;
                if (remaining > 0) {
                    int chunk = (int)(remaining > WRITE_CHUNK_BYTES
                                      ? WRITE_CHUNK_BYTES
                                      : remaining);
                    int n = ssh_aux_write(v->aux,
                                          (const char *)(v->mic_buf + v->xfer_write_pos),
                                          chunk);
                    if (n < 0) {
                        release_aux(v);
                        enter_error(v, "aux write");
                        break;
                    }
                    if (n > 0) v->xfer_write_pos += (uint32_t)n;
                }
                if (v->xfer_write_pos >= v->pcm_len) v->xfer_phase = 1;
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
                /* Drain the response → main SSH channel. */
                if (v->reply_len < (int)sizeof(v->reply_buf)) {
                    int n = ssh_aux_read(v->aux,
                                         v->reply_buf + v->reply_len,
                                         (int)sizeof(v->reply_buf) - v->reply_len);
                    if (n < 0) {
                        release_aux(v);
                        enter_error(v, "aux read");
                        break;
                    }
                    if (n > 0) v->reply_len += n;
                }
                if (ssh_aux_eof(v->aux)) {
                    if (v->reply_len > 0) {
                        ssh_write(ssh, v->reply_buf, v->reply_len);
                    }
                    release_aux(v);
                    enter_idle(v);
                }
            }
            break;
        }

        case VOICE_ERROR:
            if (v->state_frame >= ERROR_DECAY_FRAMES) enter_idle(v);
            break;
    }
}

voice_state_t voice_state(const voice_t *v) { return v ? v->state : VOICE_IDLE; }
int           voice_state_frame(const voice_t *v) { return v ? v->state_frame : 0; }

/* 8-frame Braille spinner — well-known cli-spinners "dots" pattern.
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
        case VOICE_RECORDING:    return "REC";
        case VOICE_TRANSCRIBING: return spinner_frame(v->state_frame);
        case VOICE_ERROR:        return "ERR";
        default:                 return NULL;
    }
}

uint32_t voice_status_bg(const voice_t *v) {
    if (!v) return 0;
    switch (v->state) {
        case VOICE_RECORDING:
            /* Pulsing red — alpha breathes from 0x60 to 0xc0 over 60 frames. */
            {
                int t = v->state_frame % 60;
                /* triangle wave 0..30..0 → alpha 0x60..0xc0..0x60 */
                int up = (t < 30) ? t : (60 - t);
                uint8_t alpha = 0x60 + (uint8_t)(up * 2);  /* up*2 = 0..60 */
                return 0xf7768e00u | alpha;
            }
        case VOICE_TRANSCRIBING: return 0x7dcfc080u;  /* tn cyan, semi */
        case VOICE_ERROR:        return 0xf7768eff;   /* tn red, opaque */
        default:                 return 0;
    }
}

uint32_t voice_status_fg(const voice_t *v) {
    if (!v) return 0xc0caf5ff;
    switch (v->state) {
        case VOICE_RECORDING:
        case VOICE_ERROR:
        case VOICE_TRANSCRIBING:
            /* Dark text on bright background = readable. */
            return 0x1a1b26ff;
        default:
            return 0xc0caf5ff;
    }
}
