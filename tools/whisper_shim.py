#!/usr/bin/env python3
"""dssh-whisper-shim — dual-track dispatcher between OpenRouter API and
the local self-hosted whisper.cpp daemon, with an optional `--ask`
mode that pipes the transcribed question into a DeepSeek chat
completion and returns the answer.

Without --ask  (default, used by the 3DS voice-IME path):
    Read PCM16 LE mono 16 kHz from stdin → transcribe → write Chinese
    UTF-8 text to stdout.  Routed by the active "track" config:

      ~/.config/dssh-whisper/track == "api"   → OpenRouter Whisper Turbo
      ~/.config/dssh-whisper/track == "local" → /tmp/dssh-whisper.sock

With --ask  (used by the 3DS L+START AI-question modal):
    Read a length-prefixed binary blob:
        4 bytes  PCM length (big-endian uint32)
        N bytes  PCM16 LE mono 16 kHz
        rest     UTF-8 history JSON, e.g. [["Q1","A1"], ["Q2","A2"]]
                 (an empty array `[]` means no prior context)

    Transcribe the PCM via the configured audio track, then call
    DeepSeek chat completions with the constructed messages and write
    a single JSON line to stdout:
        {"question": "<transcribed text>", "answer": "<DeepSeek reply>"}

    Exits non-zero on any failure with a short stderr diagnostic.

API key files (chmod 0600):
    ~/.config/dssh-whisper/api-key       — OpenRouter (audio)
    ~/.config/dssh-whisper/deepseek-key  — DeepSeek   (chat)

Stdlib-only on purpose so SSH-exec startup stays in the tens of ms.
"""
from __future__ import annotations

import argparse
import base64
import json
import socket
import struct
import sys
import urllib.error
import urllib.request
from pathlib import Path

CONFIG_DIR    = Path.home() / ".config" / "dssh-whisper"
TRACK_FILE    = CONFIG_DIR / "track"
API_KEY_FILE  = CONFIG_DIR / "api-key"
DS_KEY_FILE   = CONFIG_DIR / "deepseek-key"
SOCK_PATH     = "/tmp/dssh-whisper.sock"

OPENROUTER_AUDIO_URL = "https://openrouter.ai/api/v1/audio/transcriptions"
OPENROUTER_AUDIO_MODEL = "openai/whisper-large-v3-turbo"
AUDIO_LANG = "zh"
AUDIO_TIMEOUT_SEC = 30

DEEPSEEK_URL    = "https://api.deepseek.com/chat/completions"
DEEPSEEK_MODEL  = "deepseek-chat"
DEEPSEEK_TIMEOUT_SEC = 30
DEEPSEEK_SYSTEM_PROMPT = (
    "Answer the user's question thoroughly in 6-15 sentences.  "
    "Match the question's language (中文问题 → 中文回答).  "
    "No preamble, no sign-off."
)


# ────────────────────────────────────────────────────────────────────
def _err(msg: str) -> None:
    sys.stderr.write(f"[dssh-whisper-shim] {msg}\n")


# ── Diagnostic log for --ask debugging ─────────────────────────────
# Every --ask invocation appends a timestamped block to /tmp/dssh-ai-
# debug.log so we can see what the 3DS actually sent and what we
# replied — the SSH client's terminal swallows stderr after the
# channel closes, so this is the only way to debug field-cases.
import datetime, os
DEBUG_LOG = "/tmp/dssh-ai-debug.log"

def _dlog(msg: str) -> None:
    try:
        with open(DEBUG_LOG, "a") as f:
            ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            f.write(f"[{ts}] {msg}\n")
    except OSError:
        pass


def _emit_ask_json(question: str, answer: str) -> None:
    """Single point where --ask writes JSON to stdout.  Always called
    on every code path so the 3DS never receives an empty reply."""
    out = json.dumps({"question": question, "answer": answer},
                     ensure_ascii=False)
    sys.stdout.write(out)
    sys.stdout.flush()
    _dlog(f"  out: {out[:200]}")


def _get_track() -> str:
    try:
        return (TRACK_FILE.read_text().strip() or "api").lower()
    except FileNotFoundError:
        return "api"
    except OSError as exc:
        _err(f"track config unreadable ({exc}); defaulting to api")
        return "api"


def _pcm_to_wav(pcm: bytes,
                sample_rate: int = 16000,
                channels: int = 1,
                bits: int = 16) -> bytes:
    byte_rate   = sample_rate * channels * bits // 8
    block_align = channels * bits // 8
    data_size   = len(pcm)
    return (
        b"RIFF" + struct.pack("<I", 36 + data_size) + b"WAVE"
        + b"fmt " + struct.pack(
            "<IHHIIHH", 16, 1, channels, sample_rate,
            byte_rate, block_align, bits)
        + b"data" + struct.pack("<I", data_size) + pcm
    )


# ── Audio: API track via OpenRouter ────────────────────────────────
def _transcribe_via_api(pcm: bytes) -> str | None:
    """Returns transcribed text on success, None on failure (logs to stderr)."""
    try:
        key = API_KEY_FILE.read_text().strip()
    except FileNotFoundError:
        _err(f"no audio API key at {API_KEY_FILE}")
        return None
    if not key:
        _err(f"empty API key at {API_KEY_FILE}")
        return None

    body = json.dumps({
        "model": OPENROUTER_AUDIO_MODEL,
        "language": AUDIO_LANG,
        "input_audio": {
            "data": base64.b64encode(_pcm_to_wav(pcm)).decode("ascii"),
            "format": "wav",
        },
    }).encode("utf-8")

    req = urllib.request.Request(
        OPENROUTER_AUDIO_URL,
        data=body,
        method="POST",
        headers={
            "Authorization": f"Bearer {key}",
            "Content-Type": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=AUDIO_TIMEOUT_SEC) as resp:
            payload = json.loads(resp.read())
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")[:200]
        _err(f"audio API HTTP {exc.code}: {body}")
        return None
    except urllib.error.URLError as exc:
        _err(f"audio network error: {exc.reason}")
        return None
    except (TimeoutError, socket.timeout):
        _err("audio API timeout (>30s)")
        return None
    except Exception as exc:  # noqa: BLE001
        _err(f"audio API parse error: {exc}")
        return None

    return (payload.get("text") or "").strip() or None


# ── Audio: local track via Unix socket ─────────────────────────────
def _transcribe_via_local(pcm: bytes) -> str | None:
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(SOCK_PATH)
    except (FileNotFoundError, ConnectionRefusedError) as exc:
        _err(f"local daemon not running ({exc})")
        return None
    s.sendall(pcm)
    s.shutdown(socket.SHUT_WR)
    chunks = []
    while True:
        c = s.recv(4096)
        if not c:
            break
        chunks.append(c)
    text = b"".join(chunks).decode("utf-8", errors="replace").strip()
    return text or None


def _transcribe(pcm: bytes) -> str | None:
    track = _get_track()
    if track == "api":
        return _transcribe_via_api(pcm)
    if track == "local":
        return _transcribe_via_local(pcm)
    _err(f"unknown track {track!r} in {TRACK_FILE}")
    return None


# Whisper hallucination guard.  whisper-large-v3 is known to fill silent
# audio with YouTube subtitle boilerplate ("请不要吝啬您的点赞、订阅...");
# this is acknowledged by OpenAI in their model card.  We don't try to
# catch the full menagerie of subtitle templates — just the two
# unmistakable Chinese markers.  Either keyword in a transcription means
# the user almost certainly recorded silence, so we replace the output
# with a literal "empty" sentinel.
def _is_hallucination(text: str) -> bool:
    return bool(text) and ("吝啬" in text or "点赞" in text)


# ── Chat: DeepSeek ─────────────────────────────────────────────────
def _ask_deepseek(question: str, history: list[list[str]]) -> str | None:
    """history is a list of [Q, A] pairs (oldest first).  Each entry expands
    into two messages (user, assistant) appended before the new user turn."""
    try:
        key = DS_KEY_FILE.read_text().strip()
    except FileNotFoundError:
        _err(f"no DeepSeek key at {DS_KEY_FILE}; "
             "re-run install or create it manually")
        return None
    if not key:
        _err(f"empty DeepSeek key at {DS_KEY_FILE}")
        return None

    messages: list[dict] = [{"role": "system", "content": DEEPSEEK_SYSTEM_PROMPT}]
    for pair in history or []:
        if isinstance(pair, list) and len(pair) == 2:
            q, a = pair
            messages.append({"role": "user", "content": q})
            messages.append({"role": "assistant", "content": a})
    messages.append({"role": "user", "content": question})

    body = json.dumps({
        "model": DEEPSEEK_MODEL,
        "messages": messages,
        "temperature": 0.4,
        "max_tokens": 2400,
    }).encode("utf-8")

    req = urllib.request.Request(
        DEEPSEEK_URL,
        data=body,
        method="POST",
        headers={
            "Authorization": f"Bearer {key}",
            "Content-Type": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=DEEPSEEK_TIMEOUT_SEC) as resp:
            payload = json.loads(resp.read())
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")[:200]
        _err(f"DeepSeek HTTP {exc.code}: {body}")
        return None
    except urllib.error.URLError as exc:
        _err(f"DeepSeek network error: {exc.reason}")
        return None
    except (TimeoutError, socket.timeout):
        _err("DeepSeek API timeout (>30s)")
        return None
    except Exception as exc:  # noqa: BLE001
        _err(f"DeepSeek parse error: {exc}")
        return None

    try:
        return payload["choices"][0]["message"]["content"].strip()
    except (KeyError, IndexError, AttributeError):
        _err(f"DeepSeek malformed response: {json.dumps(payload)[:200]}")
        return None


# ── Mode dispatch ──────────────────────────────────────────────────
def _run_default() -> int:
    """Track-routed transcription only.  Used by voice IME path."""
    pcm = sys.stdin.buffer.read()
    if len(pcm) < 1024:
        _err(f"PCM too short ({len(pcm)} bytes); ignored")
        return 1
    text = _transcribe(pcm)
    if text is None:
        return 1
    if _is_hallucination(text):
        text = "empty"
    sys.stdout.write(text)
    sys.stdout.flush()
    return 0


def _run_ask() -> int:
    """Length-prefixed PCM + history JSON → DeepSeek answer JSON.

    Every exit path emits a JSON object on stdout via _emit_ask_json so
    the 3DS-side parser always sees a parseable response (worst-case
    one with a useful `[error]` answer field), never an empty channel.
    """
    _dlog("=" * 60)
    _dlog("--ask invoked")

    raw_len = sys.stdin.buffer.read(4)
    if len(raw_len) != 4:
        _err("--ask: short read on PCM length header")
        _dlog(f"  ABORT: short header read ({len(raw_len)}/4)")
        _emit_ask_json("", "[shim: short PCM length header]")
        return 1
    (pcm_len,) = struct.unpack(">I", raw_len)
    _dlog(f"  PCM length header = {pcm_len}")
    if pcm_len < 1024 or pcm_len > 16 * 1024 * 1024:
        _err(f"--ask: implausible PCM length {pcm_len}")
        _dlog(f"  ABORT: implausible PCM length")
        _emit_ask_json("", f"[shim: bad PCM length {pcm_len}]")
        return 1

    # Read PCM in chunks — stdin.buffer.read(N) on SSH-piped input may
    # return fewer than N bytes per call; loop until we have all PCM
    # bytes or stdin closes.
    pcm_chunks = []
    remaining = pcm_len
    while remaining > 0:
        chunk = sys.stdin.buffer.read(remaining)
        if not chunk:
            break
        pcm_chunks.append(chunk)
        remaining -= len(chunk)
    pcm = b"".join(pcm_chunks)
    _dlog(f"  PCM bytes received = {len(pcm)} (wanted {pcm_len})")
    if len(pcm) != pcm_len:
        _err(f"--ask: short PCM read ({len(pcm)}/{pcm_len})")
        _emit_ask_json("", f"[shim: short PCM ({len(pcm)}/{pcm_len})]")
        return 1

    history_raw = sys.stdin.buffer.read().decode("utf-8", errors="replace").strip()
    _dlog(f"  history_raw = {history_raw[:120]!r}")
    if not history_raw:
        history_raw = "[]"
    try:
        history = json.loads(history_raw)
        if not isinstance(history, list):
            raise ValueError("history must be a JSON array")
    except Exception as exc:  # noqa: BLE001
        _err(f"--ask: bad history JSON ({exc}); ignoring")
        _dlog(f"  WARN: history JSON parse failed ({exc}); using []")
        history = []
    _dlog(f"  history turns = {len(history)}")

    question = _transcribe(pcm)
    _dlog(f"  question = {question!r}")
    if not question:
        _emit_ask_json("", "[transcription failed]")
        return 1

    if _is_hallucination(question):
        # Silence in → YouTube boilerplate out.  Skip the DeepSeek call
        # entirely (don't pollute history, don't burn a token) and emit
        # a literal "empty" sentinel for both the question and answer
        # so the modal makes the empty-input case obvious.
        _dlog("  → hallucination markers detected; emitting 'empty'")
        _emit_ask_json("empty", "empty")
        return 0

    answer = _ask_deepseek(question, history)
    _dlog(f"  answer = {(answer or '')[:200]!r}")
    if not answer:
        _emit_ask_json(question, "[DeepSeek call failed; check daemon stderr]")
        return 1

    _emit_ask_json(question, answer)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--ask", action="store_true",
                        help="length-prefixed mode: PCM + history JSON → "
                             "transcribe + chat → JSON output")
    args, _ = parser.parse_known_args()
    return _run_ask() if args.ask else _run_default()


if __name__ == "__main__":
    sys.exit(main())
