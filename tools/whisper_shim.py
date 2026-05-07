#!/usr/bin/env python3
"""dssh-whisper-shim — dual-track dispatcher between OpenRouter API and
the local self-hosted whisper.cpp daemon.

Invoked over SSH `exec ~/.local/bin/dssh-whisper-shim` from the 3DS.
Reads PCM16 little-endian mono 16 kHz audio from stdin (no length
prefix; rely on EOF), routes to the active "track" — chosen via
`~/.config/dssh-whisper/track` (one of "api" or "local") — and prints
the transcribed Chinese UTF-8 text to stdout.

  track=api    → POST as base64-WAV to OpenRouter Whisper Large V3
                 Turbo at /api/v1/audio/transcriptions.  Reads the key
                 from `~/.config/dssh-whisper/api-key` (chmod 0600).
                 Typical end-to-end latency: 1-2 s.

  track=local  → Forward bytes to /tmp/dssh-whisper.sock (the
                 systemd-managed whisper.cpp daemon).  Latency depends
                 on server load and model size.

Exits 0 on success, 1 on any error (key missing, API failure, daemon
unreachable, etc.) with a short diagnostic on stderr.

Stdlib-only on purpose so SSH-exec startup cost stays in the tens of
milliseconds.  No requests / openai SDK / numpy.
"""
from __future__ import annotations

import base64
import json
import socket
import struct
import sys
import urllib.error
import urllib.request
from pathlib import Path

CONFIG_DIR = Path.home() / ".config" / "dssh-whisper"
TRACK_FILE = CONFIG_DIR / "track"
KEY_FILE   = CONFIG_DIR / "api-key"
SOCK_PATH  = "/tmp/dssh-whisper.sock"

API_URL    = "https://openrouter.ai/api/v1/audio/transcriptions"
API_MODEL  = "openai/whisper-large-v3-turbo"
API_LANG   = "zh"
API_TIMEOUT_SEC = 30


def _err(msg: str) -> None:
    sys.stderr.write(f"[dssh-whisper-shim] {msg}\n")


def _get_track() -> str:
    """Read the active track; default 'api' if config absent."""
    try:
        return TRACK_FILE.read_text().strip() or "api"
    except FileNotFoundError:
        return "api"
    except OSError as exc:
        _err(f"track config unreadable ({exc}); defaulting to api")
        return "api"


def _pcm_to_wav(pcm: bytes,
                sample_rate: int = 16000,
                channels: int = 1,
                bits: int = 16) -> bytes:
    """Wrap raw little-endian signed PCM16 bytes in a minimal WAV
    container.  No external dependency; the format is fixed and trivial."""
    byte_rate   = sample_rate * channels * bits // 8
    block_align = channels * bits // 8
    data_size   = len(pcm)
    return (
        b"RIFF"
        + struct.pack("<I", 36 + data_size)
        + b"WAVE"
        + b"fmt "
        + struct.pack(
            "<IHHIIHH",
            16,            # fmt chunk size (PCM)
            1,             # audio format (PCM)
            channels,
            sample_rate,
            byte_rate,
            block_align,
            bits,
        )
        + b"data"
        + struct.pack("<I", data_size)
        + pcm
    )


def _via_api(pcm: bytes) -> int:
    """Track 'api' — base64-WAV → OpenRouter."""
    try:
        key = KEY_FILE.read_text().strip()
    except FileNotFoundError:
        _err(f"no API key at {KEY_FILE}; "
             "either create it or run `dssh-whisper switch local`")
        return 1
    if not key:
        _err(f"empty API key at {KEY_FILE}")
        return 1

    body = json.dumps({
        "model": API_MODEL,
        "language": API_LANG,
        "input_audio": {
            "data": base64.b64encode(_pcm_to_wav(pcm)).decode("ascii"),
            "format": "wav",
        },
    }).encode("utf-8")

    req = urllib.request.Request(
        API_URL,
        data=body,
        method="POST",
        headers={
            "Authorization": f"Bearer {key}",
            "Content-Type": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=API_TIMEOUT_SEC) as resp:
            payload = json.loads(resp.read())
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")[:200]
        _err(f"API HTTP {exc.code}: {body}")
        return 1
    except urllib.error.URLError as exc:
        _err(f"API network error: {exc.reason}")
        return 1
    except (TimeoutError, socket.timeout):
        _err("API timeout (>30s)")
        return 1
    except Exception as exc:  # noqa: BLE001
        _err(f"API parse error: {exc}")
        return 1

    text = (payload.get("text") or "").strip()
    if not text:
        _err(f"API returned empty text; full response: {json.dumps(payload)[:200]}")
        return 1
    sys.stdout.write(text)
    sys.stdout.flush()
    return 0


def _via_local(pcm: bytes) -> int:
    """Track 'local' — forward bytes to the self-hosted daemon socket."""
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(SOCK_PATH)
    except (FileNotFoundError, ConnectionRefusedError) as exc:
        _err(f"local daemon not running ({exc}); "
             "start with `dssh-whisper start` or "
             "`dssh-whisper switch api` to use the cloud track")
        return 1

    s.sendall(pcm)
    s.shutdown(socket.SHUT_WR)
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        sys.stdout.buffer.write(chunk)
    sys.stdout.buffer.flush()
    return 0


def main() -> int:
    pcm = sys.stdin.buffer.read()
    if len(pcm) < 1024:
        _err(f"PCM too short ({len(pcm)} bytes); ignored")
        return 1

    track = _get_track().lower()
    if track == "api":
        return _via_api(pcm)
    if track == "local":
        return _via_local(pcm)
    _err(f"unknown track {track!r} in {TRACK_FILE}; expected 'api' or 'local'")
    return 1


if __name__ == "__main__":
    sys.exit(main())
