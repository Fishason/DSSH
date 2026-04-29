# 3dssh — Nintendo 3DS SSH client (Chinese)

A Nintendo 3DS native SSH terminal client with on-screen Pinyin IME, designed for using `tmux` + Claude Code over SSH from a homebrew-enabled 3DS.

**Status: M0** — toolchain skeleton. See `MILESTONES.md` for roadmap.

## Quick start

```bash
# Env (already in /etc/profile.d/devkit-env.sh on this dev host; new shells auto-load):
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM
export PATH=/opt/devkitpro/tools/bin:$PATH

# 1. Build:
make

# 2. Push to a real 3DS via WiFi (3DS must be running Homebrew Launcher's net loader):
3dslink 3dssh.3dsx

# 3. Or copy 3dssh.3dsx + 3dssh.smdh to SD card /3ds/3dssh/ and launch via HBL.
```

## Architecture (target, after M9)

```
Top screen (400x240)               Bottom screen (320x240)
┌─────────────────────────────┐    ┌─────────────────────────┐
│ SSH terminal (xterm-256)    │    │ Pinyin candidate bar    │
│ - tmux, claude code         │    ├─────────────────────────┤
│ - 256-color + TrueColor     │    │                         │
│ - 12x12 Chinese bitmap font │    │ Soft keyboard           │
│ - 500-line scrollback       │    │ (page 1: zh/en QWERTY,  │
│                             │    │  page 2: symbols/F-keys)│
└─────────────────────────────┘    └─────────────────────────┘
       Physical buttons:
       D-pad → arrow keys, A → Enter, B → Backspace,
       X → Tab, Y → toggle keyboard visibility,
       START → Esc, SELECT → Ctrl-lock,
       L+key → Ctrl combos, R → switch keyboard page,
       Circle Pad → scrollback.
```

## Build environment

devkitPro is installed natively at `/opt/devkitpro` (devkitARM release 65, GCC 14.2.0).
Env vars are exported via `/etc/profile.d/devkit-env.sh`. Requirements on host:

- Linux (tested Ubuntu 22.04, x86_64)
- ~1 GB free disk for /opt/devkitpro + libssh2 build artifacts (M1+)
- A modded 3DS for end-to-end testing

### Why no docker / Azahar in the dev loop

Originally we considered Azahar 3DS emulator for self-verification on the dev box.
Azahar AppImage requires GLIBC 2.38+ (Ubuntu 24.04), this host is 22.04 / 2.35.
Working around it (Ubuntu 24.04 docker container running Azahar with software OpenGL
under Xvfb) is high-effort, low-return for our use case. Instead the dev loop is:

**build natively** → **build-level checks** → **real 3DS via 3dslink/SD card**.

## Project layout

```
.
├── Makefile              # Top-level build (delegates to devkitARM rules)
├── source/               # C source code
├── include/              # Public headers (none yet)
├── data/                 # Static binary assets
├── romfs/                # RomFS contents (font atlas, IME dict — generated)
├── tools/
│   ├── dkp.sh            # docker wrapper for build commands
│   ├── gen_font.py       # (M6) bitmap font atlas generator
│   └── gen_pinyin_dict.py# (M7) Pinyin IME dictionary builder
├── sd_template/          # Files to copy to user's 3DS SD card
└── build/                # Build outputs (gitignored)
```

## Inspiration

Architecture inspired by [skmtrd/3dssh](https://github.com/skmtrd/3dssh) (Japanese,
Claude Code tuned). This project replaces Japanese fonts with Chinese, adds a
Pinyin IME, and uses RSA-4096 public key authentication instead of password.
