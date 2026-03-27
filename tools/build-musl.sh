#!/bin/bash
# build-musl.sh — Build musl as a shared library for dynamic linking.
#
# Output:
#   build/musl-dynamic/lib/libc.so                — shared library + interpreter
#   build/musl-dynamic/lib/ld-musl-x86_64.so.1    — symlink to libc.so
#   build/musl-dynamic/usr/bin/musl-gcc            — gcc wrapper for dynamic linking
#   build/musl-dynamic/usr/lib/musl-gcc.specs      — gcc specs file
#
# Must run on Linux (musl builds for the host architecture).

set -euo pipefail

MUSL_VER=1.2.5
MUSL_URL="https://musl.libc.org/releases/musl-${MUSL_VER}.tar.gz"
MUSL_TAR="references/musl-${MUSL_VER}.tar.gz"
MUSL_SRC="references/musl-${MUSL_VER}"
DESTDIR="$(pwd)/build/musl-dynamic"

# Skip if already built
if [ -f "${DESTDIR}/usr/lib/libc.so" ]; then
    echo "[build-musl] libc.so already exists, skipping."
    exit 0
fi

# Download if absent
if [ ! -d "${MUSL_SRC}" ]; then
    mkdir -p references
    if [ ! -f "${MUSL_TAR}" ]; then
        echo "[build-musl] Downloading musl ${MUSL_VER}..."
        curl -L -o "${MUSL_TAR}" "${MUSL_URL}"
    fi
    echo "[build-musl] Extracting..."
    tar -xzf "${MUSL_TAR}" -C references/
fi

# Configure
cd "${MUSL_SRC}"
if [ ! -f config.mak ]; then
    echo "[build-musl] Configuring..."
    ./configure \
        --prefix=/usr \
        --syslibdir=/lib \
        --enable-shared \
        CFLAGS="-O2 -fno-pie"
fi

# Build
echo "[build-musl] Building..."
make -j"$(nproc)"

# Install to DESTDIR
echo "[build-musl] Installing to ${DESTDIR}..."
make install DESTDIR="${DESTDIR}"

# Fix up paths: specs file and wrapper reference /usr/{lib,include} but we
# installed to $DESTDIR/usr/{lib,include}. Patch them to use absolute DESTDIR paths.
echo "[build-musl] Fixing up specs and wrapper paths..."
SPECS="${DESTDIR}/usr/lib/musl-gcc.specs"
sed -i "s|/usr/lib|${DESTDIR}/usr/lib|g; s|/usr/include|${DESTDIR}/usr/include|g" "$SPECS"
# Also fix the dynamic linker path in specs to point to DESTDIR for HOST linking
# (the -dynamic-linker /lib/ld-musl-x86_64.so.1 stays as /lib/ — that's the RUNTIME path
# inside the guest kernel, not the host path)

# Fix the wrapper to point to our specs file
WRAPPER="${DESTDIR}/usr/bin/musl-gcc"
cat > "$WRAPPER" << WEOF
#!/bin/sh
exec "\${REALGCC:-gcc}" "\$@" -specs "${SPECS}"
WEOF
chmod +x "$WRAPPER"

echo "[build-musl] Done. libc.so at ${DESTDIR}/usr/lib/libc.so"
