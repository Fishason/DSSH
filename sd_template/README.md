# SD card setup for DSSH

This directory mirrors what should end up on your 3DS's SD card after you
install the `.cia` (or copy the `.3dsx`).  See the project root README for
the full install + usage guide.

## One-time server setup (do this on your PC)

The libssh2 + mbedTLS stack on 3DS does NOT support ed25519 keys.  Add a
new RSA-4096 key alongside any existing keys — the original keys keep
working from your PC, and the 3DS uses RSA only.

> ⚠️ **Pass `-m PEM`.**  devkitPro's `3ds-mbedtls` is pinned at 2.28.x
> and can only parse traditional PEM private keys
> (`-----BEGIN RSA PRIVATE KEY-----`).  Newer `ssh-keygen` defaults to
> the OpenSSH format (`-----BEGIN OPENSSH PRIVATE KEY-----`) which
> mbedtls 2.28 cannot read — DSSH will fail at handshake without it.

```bash
# 1. Generate a fresh 3DS-only RSA key (-m PEM = traditional format)
ssh-keygen -t rsa -b 4096 -m PEM -f ~/.ssh/id_rsa_3ds -C "3ds-ssh-client"

# 2. Add the public half to the server's authorized_keys
ssh-copy-id -i ~/.ssh/id_rsa_3ds.pub user@your-server.example.com

# 3. Verify
ssh -i ~/.ssh/id_rsa_3ds user@your-server.example.com 'echo OK'
```

If you already have a 3DS RSA key in OpenSSH format, convert it in
place (no need to re-add the public half):

```bash
ssh-keygen -p -m PEM -f ~/.ssh/id_rsa_3ds
# After conversion: head -1 ~/.ssh/id_rsa_3ds  → -----BEGIN RSA PRIVATE KEY-----
```

Optional but recommended: prepend the line in the server's
`~/.ssh/authorized_keys` with `from="<your-home-public-IP>"` so a lost SD
card can only log in from your home network.

## Files to copy to SD card

Insert the SD card into your PC and copy these into `/3ds/3dssh/`:

| File          | Source                        | Notes                       |
|---------------|-------------------------------|-----------------------------|
| `config.ini`  | rename `config.ini.example`   | edit values for your server |
| `id_rsa`      | `~/.ssh/id_rsa_3ds`           | the RSA private key         |

Final SD layout:

```
sd:/3ds/3dssh/
├── config.ini
└── id_rsa
```

If you installed via `DSSH.cia`, the executable + pinyin dict are
already inside the title — you only need these two files on the SD.
