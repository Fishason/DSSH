#!/bin/bash
# Cross-compile libssh2 for 3DS (armv6k, mbedTLS backend).
# Patches 3DS newlib's missing struct iovec and 3DS-specific size_t/unsigned-long
# mismatches in libssh2's mbedtls backend.
#
# Adapted from skmtrd/3dssh's build-libssh2.sh (originally macOS-only).
# This version: Linux, ubuntu user owns /opt/devkitpro/portlibs (no sudo install).

set -e

export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=${DEVKITARM:-$DEVKITPRO/devkitARM}
export PATH=$DEVKITARM/bin:$DEVKITPRO/portlibs/3ds/bin:$DEVKITPRO/tools/bin:$PATH

PORTLIBS=$DEVKITPRO/portlibs/3ds
LIBSSH2_VERSION=1.11.0
LIBSSH2_URL="https://github.com/libssh2/libssh2/releases/download/libssh2-${LIBSSH2_VERSION}/libssh2-${LIBSSH2_VERSION}.tar.gz"
WORKDIR=/tmp/libssh2-build
SRC=$WORKDIR/libssh2-${LIBSSH2_VERSION}
COMPAT_HEADER=$WORKDIR/compat_3ds.h

mkdir -p "$WORKDIR"
cd "$WORKDIR"

echo ">>> Downloading libssh2 ${LIBSSH2_VERSION}..."
if [ ! -f libssh2.tar.gz ]; then
    curl -L -o libssh2.tar.gz "$LIBSSH2_URL"
fi
if [ ! -d "$SRC" ]; then
    tar xf libssh2.tar.gz
fi

# 3DS newlib lacks sys/uio.h / struct iovec. Define it for libssh2's internals.
cat > "$COMPAT_HEADER" << 'EOF'
#ifndef COMPAT_3DS_H
#define COMPAT_3DS_H
#ifndef _SYS_UIO_H
#ifndef _STRUCT_IOVEC
#define _STRUCT_IOVEC
struct iovec {
    void         *iov_base;
    unsigned int  iov_len;
};
#endif
#endif
#endif /* COMPAT_3DS_H */
EOF
echo ">>> Created $COMPAT_HEADER"

cd "$SRC"
echo ">>> Patching unsigned long -> size_t (ARM 32-bit type mismatch)..."
python3 - <<'PYEOF'
import re

files_to_patch = ["src/mbedtls.h", "src/mbedtls.c", "src/crypto.h"]

def patch_unsigned_long(content):
    return re.sub(
        r'\bunsigned long\b(\s+)(\w)',
        lambda m: 'size_t' + m.group(1) + m.group(2),
        content,
    )

for path in files_to_patch:
    try:
        with open(path) as f:
            original = f.read()
    except FileNotFoundError:
        print(f"  SKIP (not found): {path}")
        continue
    if "/* 3DS size_t patch applied */" in original:
        print(f"  Already patched: {path}")
        continue
    patched = patch_unsigned_long(original)
    if patched != original:
        patched = "/* 3DS size_t patch applied */\n" + patched
        with open(path, "w") as f:
            f.write(patched)
        print(f"  Patched: {path}")
    else:
        print(f"  No unsigned long found: {path}")
PYEOF

echo ">>> Patching uninitialized 'ret' in _libssh2_mbedtls_pub_priv_key()..."
# libssh2 1.11.0 src/mbedtls.c: 'int ret;' is left uninitialized; the function
# returns it as a stack-garbage value when both mth + key allocations succeed.
# Fixed in master but not in 1.11.0 release. Manifests on ARM (no stack zero
# init) as the infamous "auth fn=-1 sess=0" with no useful error message.
# Reference: https://github.com/libssh2/libssh2/blob/master/src/mbedtls.c
python3 - <<'PYEOF'
import re
path = "src/mbedtls.c"
with open(path) as f:
    src = f.read()

if "/* 3DS uninit-ret patch applied */" in src:
    print("  Already patched: ret-init")
else:
    # Find:  '    int ret;\n    mbedtls_rsa_context *rsa;'  (1.11.0)
    # Replace with:  '    int ret = 0;\n    mbedtls_rsa_context *rsa;'
    # only inside _libssh2_mbedtls_pub_priv_key (not other functions).
    pattern = re.compile(
        r'(_libssh2_mbedtls_pub_priv_key\([^)]*\)\s*\{[^}]*?)\n'
        r'(\s+)int ret;\n'
        r'(\s+mbedtls_rsa_context \*rsa;)',
        re.DOTALL,
    )
    new_src, n = pattern.subn(
        r'\1\n\2int ret = 0;  /* 3DS uninit-ret patch applied */\n\3',
        src,
    )
    if n == 1:
        with open(path, "w") as f:
            f.write(new_src)
        print(f"  Patched: ret-init in {path}")
    else:
        raise SystemExit(
            f"FATAL: expected exactly 1 match, found {n}. "
            "libssh2 source layout may have changed."
        )
PYEOF

rm -rf build-3ds
mkdir build-3ds
cd build-3ds

echo ">>> Configuring (cmake)..."
cmake .. \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake \
    -DCMAKE_INSTALL_PREFIX="$PORTLIBS" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_C_FLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations -ffunction-sections -D__3DS__ -include $COMPAT_HEADER" \
    -DCRYPTO_BACKEND=mbedTLS \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_TESTING=OFF \
    -DENABLE_ZLIB_COMPRESSION=OFF \
    -DENABLE_DEBUG_LOGGING=OFF \
    -Dmbedcrypto_INCLUDE_DIR="$PORTLIBS/include" \
    -Dmbedcrypto_LIBRARY="$PORTLIBS/lib/libmbedcrypto.a" \
    -Dmbedtls_INCLUDE_DIR="$PORTLIBS/include" \
    -Dmbedtls_LIBRARY="$PORTLIBS/lib/libmbedtls.a" \
    -Dmbedx509_INCLUDE_DIR="$PORTLIBS/include" \
    -Dmbedx509_LIBRARY="$PORTLIBS/lib/libmbedx509.a"

echo ">>> Building..."
make -j"$(nproc)"

echo ">>> Installing to $PORTLIBS ..."
make install

echo ""
echo ">>> Done!"
ls -lh "$PORTLIBS/lib/libssh2.a"
