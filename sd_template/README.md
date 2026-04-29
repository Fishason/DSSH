# SD card setup for 3dssh

This directory mirrors what should end up on your 3DS's SD card.

## One-time server setup (do this on your PC)

The libssh2 + mbedTLS stack on 3DS does NOT support ed25519 keys. You'll add a
new RSA-4096 key alongside your existing ed25519 — the original ed25519 keeps
working from your PC, and the 3DS uses RSA only.

```bash
# 1. Generate a fresh 3DS-only RSA key
ssh-keygen -t rsa -b 4096 -f ~/.ssh/id_rsa_3ds -C "3ds-ssh-client"

# 2. Add the public half to the server's authorized_keys
ssh-copy-id -i ~/.ssh/id_rsa_3ds.pub ubuntu@52.76.104.33

# 3. Verify
ssh -i ~/.ssh/id_rsa_3ds ubuntu@52.76.104.33 'echo OK'
```

Optional but recommended for security: edit the server's `~/.ssh/authorized_keys`
and prepend the new RSA key line with `from="<your-home-public-IP>"` so even if
the SD card is lost, the key only works from your home network.

## Files to copy to SD card

Insert the SD card into your PC and copy these files into `/3ds/3dssh/`:

| File                | Source                      | Notes                          |
|---------------------|-----------------------------|--------------------------------|
| `3dssh.3dsx`        | project root after `make`   | the executable                 |
| `3dssh.smdh`        | project root after `make`   | icon + metadata                |
| `config.ini`        | rename `config.ini.example` | edit values for your server    |
| `id_rsa`            | `~/.ssh/id_rsa_3ds`         | the RSA private key            |

Final SD layout:

```
sd:/3ds/3dssh/
├── 3dssh.3dsx
├── 3dssh.smdh
├── config.ini
└── id_rsa
```

## Launching

Insert the SD card back into the 3DS and start Homebrew Launcher. The "3DS SSH
Client" entry should appear. Select it and:

- It reads `config.ini` for connection details.
- It connects to the server with public-key auth (key_path from config).
- Top screen shows the SSH session (50 cols × 30 rows in M1).
- Bottom screen shows status / instructions.

### M1 controls

- **X** — open soft keyboard, type a line, OK to send (Cancel to abort)
- **A** — Enter
- **B** — Backspace
- **D-pad** — arrow keys
- **L** — Ctrl-C (interrupt)
- **R** — Ctrl-D (logout / EOF)
- **START** — disconnect and exit to Homebrew Launcher

(Real soft keyboard with Pinyin IME comes in M4–M7.)

## Iteration via 3dslink (no SD card swapping)

After the first SD setup, you can rebuild + push without removing the SD card:

```bash
# On 3DS: open Homebrew Launcher, press Y to enter "Waiting for 3dslink..."
# On dev box:
make
3dslink -a <3DS-LAN-IP> 3dssh.3dsx
```

The 3DS still reads `config.ini` and `id_rsa` from the SD — you only re-push
the executable.
