#!/usr/bin/env python3
"""dssh-whisper-daemon — long-running whisper.cpp transcription server.

Listens on /tmp/dssh-whisper.sock for incoming PCM16 little-endian mono
16 kHz audio streams.  Each connection: read all bytes until client
shuts down its write half, transcribe via the preloaded whisper small
multilingual model with language='zh', send Chinese UTF-8 text back,
close.  The model is loaded once at daemon startup so per-call latency
is just the actual transcription work (~1.5-3s for a few-second clip
on a 2-vCPU host).

Run via the systemd user unit `dssh-whisper.service` (installed by
tools/install_whisper_server.sh) so the model stays in memory across
multiple voice triggers from the 3DS.
"""
import contextlib
import logging
import os
import signal
import socket
import sys
from pathlib import Path

import numpy as np
from pywhispercpp.model import Model

SOCK_PATH = "/tmp/dssh-whisper.sock"
# Multilingual; we always pass language='zh'.  Override with the env var
# DSSH_WHISPER_MODEL (e.g. 'tiny' for ~1-2 s on busy hosts but worse zh
# accuracy, 'medium' for ~10 s and near-state-of-the-art accuracy).
MODEL_NAME = os.environ.get("DSSH_WHISPER_MODEL", "small")
INSTALL_DIR = Path.home() / ".local/share/dssh-whisper"
MODELS_DIR = INSTALL_DIR / "models"
SAMPLE_RATE = 16000
MIN_PAYLOAD_BYTES = 1024  # below this is almost certainly an empty press

logging.basicConfig(
    level=logging.INFO,
    format="[whisper-daemon %(asctime)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger()


def cleanup_socket() -> None:
    with contextlib.suppress(FileNotFoundError):
        os.unlink(SOCK_PATH)


def main() -> int:
    log.info("loading whisper %r (multilingual, zh)...", MODEL_NAME)
    MODELS_DIR.mkdir(parents=True, exist_ok=True)
    model = Model(
        MODEL_NAME,
        models_dir=str(MODELS_DIR),
        n_threads=max(1, (os.cpu_count() or 2) - 0),
        print_progress=False,
        print_realtime=False,
        print_timestamps=False,
    )
    log.info("model ready; opening %s", SOCK_PATH)

    cleanup_socket()
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(SOCK_PATH)
    os.chmod(SOCK_PATH, 0o666)
    srv.listen(4)

    def _shutdown(signum: int, _frame) -> None:
        log.info("signal %d → shutdown", signum)
        cleanup_socket()
        sys.exit(0)

    signal.signal(signal.SIGTERM, _shutdown)
    signal.signal(signal.SIGINT, _shutdown)

    log.info("ready.")
    while True:
        conn, _ = srv.accept()
        try:
            data = bytearray()
            while True:
                chunk = conn.recv(8192)
                if not chunk:
                    break
                data.extend(chunk)

            if len(data) < MIN_PAYLOAD_BYTES:
                log.warning("rejecting tiny payload (%d bytes)", len(data))
                conn.sendall(b"")
                continue

            samples = (
                np.frombuffer(bytes(data), dtype=np.int16).astype(np.float32) / 32768.0
            )
            duration = len(samples) / SAMPLE_RATE
            log.info("transcribing %.2fs audio (%d samples)", duration, len(samples))

            segments = model.transcribe(samples, language="zh")
            text = "".join(seg.text for seg in segments).strip()
            log.info("→ %r", text[:80])

            conn.sendall(text.encode("utf-8"))
        except Exception as exc:  # noqa: BLE001 — daemon must never crash
            log.exception("transcribe failed")
            with contextlib.suppress(Exception):
                conn.sendall(f"[error: {exc}]".encode("utf-8"))
        finally:
            with contextlib.suppress(Exception):
                conn.close()


if __name__ == "__main__":
    sys.exit(main() or 0)
