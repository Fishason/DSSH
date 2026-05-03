<p align="center">
  <b>English</b> · <a href="README.zh.md">中文</a>
</p>

<h1 align="center">DSSH</h1>

<p align="center">
  <img src="icon.png" alt="DSSH icon" width="96" height="96">
</p>

<p align="center">
  <b>Nintendo 3DS SSH client with on-screen pinyin IME</b><br>
  Top screen runs a citro2d ANSI terminal · bottom screen draws its own
  soft keyboard · RSA public-key auth over libssh2 + mbedTLS<br>
  Run <code>tmux</code> + <code>claude-code</code> from a 3DS — code from
  the couch without ever opening the laptop.
</p>

<p align="center">
  <img alt="platform" src="https://img.shields.io/badge/platform-Nintendo%203DS-FE0016?logo=nintendo3ds&logoColor=white">
  <img alt="license" src="https://img.shields.io/badge/license-MIT-blue">
  <img alt="build" src="https://img.shields.io/badge/build-devkitARM-orange">
</p>

<p align="center">
  <img src="docs/media/preview.gif" alt="DSSH live demo" width="540"><br>
  <sub>Real New 2DS XL · top screen ANSI terminal · bottom screen soft
  keyboard + clock + crab</sub>
</p>

<p align="center">
  <a href="https://github.com/Fishason/DSSH/releases/latest/download/demo.mp4">
    Full 1m42s demo video (10 MB MP4)
  </a>
</p>

<p align="center">
  <img src="docs/media/poster.jpg" alt="Real device: typing Chinese into Claude Code via the pinyin IME" width="720"><br>
  <sub>Real New 2DS XL · top: typing「你好啊！请问您是谁，你可以做什么」into
  Claude Code · bottom: letter page + CHN mode + Shift held</sub>
</p>

---

## Features

- **Full ANSI / VT100 terminal** — tmux status bar, claude-code spinner,
  box-drawing borders, 256-color, TrueColor, Braille; everything renders.
- **Chinese rendering** — bundled Zpix 12px pixel font covers 21,000+ CJK
  unified ideographs, Terminus 6×12 for ASCII; mixed CJK/ASCII baselines
  align cleanly on the same line.
- **Self-drawn soft keyboard** — iOS-style 3px rounded keys with smooth
  press-down animation; letters / symbols pages.
- **Pinyin input method** — top 300k entries from rime-ice, plus
  abbreviation matching (`nh` → 你好), prefix fallback (`nihaoz`
  auto-falls-back to `nihao`), and a candidate cursor.
- **RSA-4096 public-key auth** — libssh2 + mbedTLS, private key read
  from the SD card.
- **Full physical-key mapping** — D-pad arrow keys, hold-style modifiers
  (L = Shift, Y = Ctrl, X = Alt), Circle Pad scrollback / mouse-wheel.
- **Anthropic-red crab mascot** — scampers along the bottom row, dodges
  when you tap it 🦀.
- **Hidden debug page** — double-tap the ENG/CHN badge to see the live
  SSH byte stream, full key-binding cheat sheet, and a mascot toggle.

## Table of contents

- [Install](#install)
- [Server-side setup](#server-side-setup-one-time)
- [Configure config.ini](#configure-configini)
- [Key bindings](#key-bindings)
- [Using the IME](#using-the-ime)
- [Debug page](#debug-page)
- [Build from source](#build-from-source)
- [Project layout](#project-layout)
- [Credits](#credits)
- [License](#license)

---

## Install

DSSH runs on a **modded 3DS / 2DS / New 3DS**.  You need either the
Homebrew Launcher (HBL) or a CIA installer like FBI.

### Option A — `.cia` install (recommended)

1. Grab `DSSH.cia` (~14 MB) from the [latest release](../../releases/latest).
2. Copy it anywhere on the SD card (e.g. `/cias/DSSH.cia`).
3. Open FBI → SD → select `DSSH.cia` → `Install CIA`.
4. The orange DSSH icon shows up on the HOME menu.

### Option B — `.3dsx` direct launch

1. Grab `3dssh.3dsx` from the latest release.
2. Copy to `/3ds/dssh/dssh.3dsx` on the SD card.
3. Open HBL → pick DSSH.

### Option C — `3dslink` over Wi-Fi (developer flow)

```bash
# On the 3DS: launch HBL, press Y → "Waiting for 3dslink..."
3dslink -a <3DS-LAN-IP> 3dssh.3dsx
```

---

## Server-side setup (one-time)

The 3DS libssh2 build uses mbedTLS as its crypto backend and
**hardcodes-disables ed25519**.  So you generate a fresh RSA-4096
keypair just for the 3DS — your existing ed25519 key on the PC keeps
working untouched:

```bash
# 1. Generate a 3DS-only RSA key on your PC
ssh-keygen -t rsa -b 4096 -f ~/.ssh/id_rsa_3ds -C "3ds-ssh-client"

# 2. Copy the public half into the server's authorized_keys
ssh-copy-id -i ~/.ssh/id_rsa_3ds.pub user@your-server.example.com

# 3. Verify the new RSA key works from your PC
ssh -i ~/.ssh/id_rsa_3ds user@your-server.example.com 'echo OK'
```

**Recommended hardening**: prepend the new line in the server's
`~/.ssh/authorized_keys` with `from="<your-home-public-IP>"` so a lost
SD card can only log in from your home network.

Copy the **private key** `~/.ssh/id_rsa_3ds` onto the SD card at
`/3ds/3dssh/id_rsa` (the path is fixed even when DSSH is installed as a
.cia — config + key always read from `sdmc:/3ds/3dssh/`).

> ⚠️ The SD card stores the key in plain text.  Anyone holding the SD
> can log in to your server.  Add `from="..."` IP restriction or
> `command="..."` lockdown in `authorized_keys`.

---

## Configure config.ini

Copy `sd_template/3ds/3dssh/config.ini.example` to the SD card at
`/3ds/3dssh/config.ini` and edit the values:

```ini
host       = your-server.example.com
port       = 22
user       = ubuntu
key_path   = sdmc:/3ds/3dssh/id_rsa
passphrase =
```

| Field | Meaning |
|---|---|
| `host` | Server IP or hostname |
| `port` | Port (default 22) |
| `user` | SSH login user |
| `key_path` | Private key path; `sdmc:/...` is the 3DS standard SD prefix |
| `passphrase` | Optional key passphrase; leave empty (typing one on the soft keyboard is awkward) |

Final SD layout:

```
sdmc:/3ds/3dssh/
├── config.ini
└── id_rsa
```

---

## Key bindings

### Physical buttons

| Button | Function | Notes |
|---|---|---|
| **A** | Enter (EN) / **emit pinyin buffer as English** (IME) | Accidentally typed English in CN mode? Press A and the buffer flies to SSH as raw ASCII — no need to backspace and switch modes. |
| **B** | Backspace / consume one pinyin letter | Hold-style auto-repeat (peaks at 60 / sec) |
| **X** | Alt modifier | Hold-style — held when the next key fires |
| **Y** | Ctrl modifier | Hold-style — Y + tap `c` → Ctrl-C |
| **L** | Shift modifier / **+ Circle Pad → right pane** | See [tmux split scrolling](#tmux-split-scrolling) below |
| **R** | Toggle CN/EN input mode | Top-right ENG/CHN reflects the current mode |
| **SELECT** | Esc | Tap fires immediately |
| **START** | Quit DSSH | |
| **Space** (soft keyboard) | Plain space (EN) / **commit highlighted candidate** (IME) | Matches sogou / fcitx convention |
| **Shift + .** | **。** (full-width Chinese period, U+3002) | Works in both EN and CN modes |
| **D-pad ↑↓** | Arrow keys / IME page nav | When the IME buffer is active, ↑↓ paginates candidates |
| **D-pad ←→** | Arrow keys / IME selection cursor | When active, ←→ moves the candidate cursor within the page |
| **Circle Pad ↑↓** | Scrollback / tmux mouse-wheel | Default targets the left/top pane; **hold L → right/bottom pane** |

> Long-press D-pad or B: 250 ms initial delay, ramps up to 12 / sec at
> 0.5 s, peaks at 60 / sec after 1.5 s.

### tmux split scrolling

The 3DS has no real cursor, so tmux's mouse-wheel events get routed by
the `(col, row)` we send.  DSSH defaults to `(1, 1)` → hits the
left/top pane; holding **L** sends `(60, 12)` → hits the right/bottom
pane.  In a vertical-split tmux:

| Action | Effect |
|---|---|
| Circle Pad ↑↓ | Scrolls the **left** pane |
| L held + Circle Pad ↑↓ | Scrolls the **right** pane |

### Soft keyboard

The bottom screen is the soft keyboard — two pages:

- **Letters page** (default): QWERTY layout with `,` `.` punctuation,
  Tab, and a wide Space.
- **Symbols page** (toggle via the bottom-left `123` key):
  `1234567890`, `!@#$%^&*()` cleanly aligned on two rows, plus other
  common punctuation including `?` and `\`.

Any key supports hold-style modifier combos.  Example:
**hold Y + tap b** = `Ctrl-B` (the tmux prefix).

### Status bar (top 30 px)

```
┌──────────────────────────────────────────────────────┐
│ [SFT]   candidate strip / pinyin buffer / cands [CHN]│
└──────────────────────────────────────────────────────┘
```

- **Left slot [STA]**: 3-letter modifier indicator (SFT/CTL/ALT stays lit
  while held; ENT/BSP/ESC/`R→C` flashes for 200 ms on transient events).
- **Middle**: pinyin buffer + candidates in CN mode; empty in EN mode.
- **Right slot [ENG/CHN]**: current IME mode.  **Double-tap to enter
  the debug page**.

---

## Using the IME

### Full pinyin

Tapping letters in CN mode brings up the candidate strip:

```
ni       → 年 你 牛奶 娘 念   (page 1/52, total 256)
nihao    → 你好 你好吗 你好啊 拟好 你好呀
shijie   → 世界 世界上 世界杯 世界各地 世界里
```

- **A** or **Space** commits the currently highlighted candidate.
- **D-pad ←→** moves the highlight within the current page.
- **D-pad ↑↓** flips between pages.
- **Tap** a candidate to commit it directly.
- **B** consumes one letter from the pinyin buffer.

### Abbreviation (initials)

Every multi-syllable word gets an extra entry keyed by its initials —
typing the initials still surfaces it (with weight × 0.3, so the
full-pinyin form still ranks first when typed in full):

```
nh → 你好  (around the 8th candidate)
wm → 我们  (top candidate)
sj → 世界
zw → 中文
xx → 谢谢
```

Page or cursor over to your target, then commit with A.

### Prefix fallback

Typed an extra letter past a valid prefix?  The engine automatically
matches the longest valid prefix and shows the surplus letters in red:

```
buffer:  niha[oz]    ← niha in green + oz in red
candidates:           still showing what nihao would produce
```

Press B to chew the red tail back to a clean prefix.

### Modifiers always bypass the IME

In CN mode, **hold Y + tap c** still sends `Ctrl-C`; **hold L + tap a**
still sends `A`.  Modifiers take priority over IME routing, so
vim / tmux / claude-code shortcuts keep working.

### Bail out: emit pinyin as English

CN mode + non-empty buffer + press **A** = the buffer flies to SSH as
raw ASCII letters and clears.  Example: you accidentally typed
`cd /etc` while in CN mode and the candidate strip is showing strange
Chinese.  One press of **A** delivers `cd /etc` to the shell — no
backspacing, no mode-toggle, no retyping.

> Difference: **Space** commits the highlighted candidate (Chinese
> chars on screen).  **A** sends the typed letters as-is.

---

## Debug page

**Double-tap the ENG/CHN badge** in the top-right corner (two taps within
500 ms) to enter the debug overlay.  Single-tap the badge again to leave.

What it shows:

- Title + exit hint.
- **recv hex**: the last 32 bytes received from SSH — for diagnosing
  ANSI / SCS / mouse-protocol issues at the byte level.
- **Physical key cheat sheet**: a condensed version of the bindings
  table above.
- **MASCOT: ON/OFF** toggle button.  Default is ON.

---

## Build from source

### Prerequisites

- Linux x86\_64 (tested on Ubuntu 22.04; other distros need the obvious
  package-name adjustments).
- [devkitPro / devkitARM](https://devkitpro.org/wiki/Getting_Started)
  release 65+, GCC 14.2.0.
- Python 3.10+ with Pillow (for font + dictionary generators).

### Steps

```bash
# 1. Install devkitPro
wget https://apt.devkitpro.org/install-devkitpro-pacman
bash install-devkitpro-pacman
sudo dkp-pacman -S 3ds-dev 3ds-mbedtls 3ds-libpng 3ds-zlib

# 2. Clone + cd
git clone https://github.com/Fishason/DSSH.git
cd DSSH

# 3. Cross-compile libssh2 (one-time, drops into $DEVKITPRO/portlibs/3ds/lib/)
bash build-libssh2.sh

# 4. Install system fonts (Terminus provides ASCII / box-drawing)
sudo apt install fonts-terminus

# 5. Fetch font sources (Zpix)
bash tools/fetch_fonts.sh

# 6. Generate the font atlas (→ source/font_data.c, ~3 MB)
python3 tools/gen_font.py

# 7. Fetch + build the pinyin dictionary (→ romfs/pinyin_dict.bin, ~13 MB)
bash tools/fetch_pinyin_dict.sh
python3 tools/gen_pinyin_dict.py

# 8. Build the .3dsx
make

# 9. (Optional) build the .cia
bash tools/install_cia_tools.sh   # installs bannertool + makerom into ~/bin
make cia                          # → DSSH.cia
```

### Test the IME engine on the host (no 3DS needed)

```bash
make test-ime
```

Compiles `tools/test_ime.c` linked against `source/ime_pinyin.c` and
runs nine smoke-test queries (`ni → 你`, `nihao → 你好`, `nh → 你好`,
etc.).

---

## Project layout

```
DSSH/
├── 69633.PNG                  # Source icon (162×102)
├── icon.png                   # 48×48 icon for .3dsx / SMDH (derived)
├── app.rsf                    # makerom CIA spec
├── Makefile                   # Top-level build (make / make cia / make test-ime)
├── build-libssh2.sh           # libssh2 + mbedTLS ARM cross-compile
├── source/
│   ├── main.c                 # Main loop, SSH receive, UTF-8 reassembly
│   ├── ssh_client.{c,h}       # libssh2 wrapper
│   ├── config.{c,h}           # SD-card config.ini parser
│   ├── terminal.{c,h}         # ANSI/VT100 parser (forked from skmtrd)
│   ├── renderer.{c,h}         # citro2d rendering (terminal, text, CJK)
│   ├── keyboard.{c,h}         # Physical buttons + IME routing
│   ├── softkb.{c,h}           # Soft keyboard + candidate strip + debug page
│   ├── ime_pinyin.{c,h}       # Pinyin engine
│   ├── mascot.{c,h}           # Crab mascot
│   ├── font_atlas.{c,h}       # Codepoint → glyph index
│   └── font_data.c            # Font bitmaps (gen_font.py output)
├── tools/
│   ├── fetch_fonts.sh         # Download Zpix
│   ├── gen_font.py            # Font atlas generator
│   ├── fetch_pinyin_dict.sh   # Download rime-ice
│   ├── gen_pinyin_dict.py     # Dictionary → binary
│   ├── test_ime.{c,sh}        # Host-side IME smoke test
│   ├── gen_cia_assets.py      # Icon / banner derivation
│   └── install_cia_tools.sh   # bannertool + makerom installer
├── romfs/                     # gitignored — packs pinyin_dict.bin
├── data/                      # gitignored — font + dict sources
└── sd_template/               # SD-card deployment template
    ├── README.md
    └── 3ds/3dssh/config.ini.example
```

## Architecture

```
SSH server (somewhere on the internet)
     ▲ libssh2 over mbedTLS-RSA-4096
     │
┌────┴──────────────────────────────────────────────────┐
│  main.c poll loop @ 60 fps                            │
│   ├─ ssh_read → softkb_record_recv → utf8 reassemble  │
│   │                ↓                                  │
│   │   terminal_write_n → ANSI parser → cell grid      │
│   ├─ hidScanInput → keyboard_handle_input             │
│   │   └─ IME mode? → ime_input_letter / page / select │
│   ├─ hidTouchRead → softkb_touch                      │
│   │   ├─ candidate strip hit → ime_select             │
│   │   ├─ key hit → keyboard_emit_for / ime_input      │
│   │   └─ badge double-tap → debug_mode toggle         │
│   └─ render: top = renderer_draw_terminal             │
│              bot = softkb_draw + clock + mascot       │
└───────────────────────────────────────────────────────┘
     │
   citro2d (3DS 2D rendering)
     │
   GPU (top 400×240 + bottom 320×240, 24-bit color)
```

The build went through milestones M0 → M9; see the commit history for
the full progression.

---

## Credits

- **[skmtrd/3dssh](https://github.com/skmtrd/3dssh)** — the original
  Japanese-localized 3DS SSH client; DSSH reuses its ANSI/VT100 parser,
  UTF-8 reassembly, and citro2d framing.
- **[rime-ice](https://github.com/iDvel/rime-ice)** — pinyin dictionary
  source (pinned at commit `3f57a6f6`).
- **[Zpix Pixel Font](https://github.com/SolidZORO/zpix-pixel-font)** —
  12 px CJK pixel font (OFL 1.1).
- **[Terminus TTF](https://terminus-font.sourceforge.net/)** — ASCII
  and box-drawing pixel font.
- **[libssh2](https://www.libssh2.org/)** + **[mbedTLS](https://www.trustedfirmware.org/projects/mbed-tls/)** —
  SSH / TLS protocol stack.
- **[devkitPro](https://devkitpro.org/) libctru / citro2d / citro3d** —
  3DS user-mode runtime and rendering.
- **[carstene1ns/3ds-bannertool](https://github.com/carstene1ns/3ds-bannertool)**
  + **[3DSGuy/Project_CTR makerom](https://github.com/3DSGuy/Project_CTR)** —
  CIA packaging tools.

## License

MIT — see [LICENSE](LICENSE).

The bundled fonts, dictionary, and upstream SSH/TLS libraries each
have their own licenses (OFL / GPL / BSD / MIT / Apache).  Respect
those when redistributing the binary.
