#!/bin/bash
# setup-pw5.sh -- cross-build Mini vMac + wario-companion + kindle-touch
# for a Kindle Paperwhite 5 (armhf, glibc 2.20) from Linux.
#
# Toolchain: Bootlin prebuilt armv7-eabihf glibc cross gcc, linked with
# --sysroot against a MERGED build sysroot (device libs + toolchain CRT
# + headers). This mirrors the proven Oasis recipe. NOTE: zig's bundled
# glibc CRT is incompatible with the device's glibc 2.20 loader and
# segfaults before main() -- do NOT switch back to zig for the glibc
# binaries.
#
# Prereqs:
#   - pw5-sysroot/ extracted from the PW5 (override with WARIO_SYSROOT)
#   - curl, tar, ar, python3, cc (for mini vMac's config generator)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SYSROOT="${WARIO_SYSROOT:-$ROOT/pw5-sysroot}"
TOOLCHAIN_DIR="$ROOT/tools/armv7-eabihf--glibc--stable-2024.02-1"
MERGED="$ROOT/tools/pw5-build-sysroot"
GCC="$TOOLCHAIN_DIR/bin/arm-buildroot-linux-gnueabihf-gcc"
GXX="$TOOLCHAIN_DIR/bin/arm-buildroot-linux-gnueabihf-g++"
STRIP="$TOOLCHAIN_DIR/bin/arm-buildroot-linux-gnueabihf-strip"
ZIG="$ROOT/tools/zig-x86_64-linux-0.16.0/zig"

[ -d "$SYSROOT/usr/lib" ] || {
	echo "ERROR: sysroot not found at $SYSROOT (set WARIO_SYSROOT)" >&2
	exit 1
}
mkdir -p "$ROOT/tools"

# 1. Bootlin glibc cross toolchain
if [ ! -x "$GCC" ]; then
	echo "==> Downloading Bootlin armv7-eabihf glibc toolchain (140M) ..."
	curl -fsSL -o "$ROOT/tools/bootlin-armv7-eabihf.tar.bz2" \
		"https://toolchains.bootlin.com/downloads/releases/toolchains/armv7-eabihf/tarballs/armv7-eabihf--glibc--stable-2024.02-1.tar.bz2"
	( cd "$ROOT/tools" && tar -xjf bootlin-armv7-eabihf.tar.bz2 && rm -f bootlin-armv7-eabihf.tar.bz2 )
fi

# 2. Merged build sysroot: device libs + toolchain CRT/headers
TSYS="$TOOLCHAIN_DIR/arm-buildroot-linux-gnueabihf/sysroot"
if [ ! -d "$MERGED" ]; then
	echo "==> Building merged sysroot (device libs + CRT + headers) ..."
	cp -a "$SYSROOT/." "$MERGED/"
	cp "$TSYS/usr/lib/crt1.o" "$TSYS/usr/lib/Scrt1.o" \
		"$TSYS/usr/lib/crti.o" "$TSYS/usr/lib/crtn.o" "$MERGED/usr/lib/"
	cp -rn "$TSYS/usr/include/." "$MERGED/usr/include/"
	cp -rn "$ROOT/tools/pw5-headers/include/." "$MERGED/usr/include/" 2>/dev/null || true
	ar rcs "$MERGED/usr/lib/libc_nonshared.a"   # toolchain gcc wants it; device lacks it
fi

# 3. cross-bin wrappers -> Bootlin gcc + merged sysroot (idempotent)
write_wrapper() {
	local name="$1" bin="$2" extra="${3:-}"
	cat > "$ROOT/cross-bin/$name" <<EOF
#!/bin/bash
SELF="\$(readlink -f "\${BASH_SOURCE[0]}")"
SELF_DIR="\$(cd "\$(dirname "\$SELF")" && pwd)"
ROOT="\$(dirname "\$SELF_DIR")"
exec "$bin" $extra "\$@"
EOF
	chmod +x "$ROOT/cross-bin/$name"
}
write_wrapper gcc   "$GCC"   "--sysroot=$MERGED"
write_wrapper g++   "$GXX"   "--sysroot=$MERGED"
write_wrapper strip "$STRIP"

# pkg-config shim against merged sysroot
cat > "$ROOT/cross-bin/pkg-config" <<EOF
#!/bin/bash
SELF="\$(readlink -f "\${BASH_SOURCE[0]}")"
SELF_DIR="\$(cd "\$(dirname "\$SELF")" && pwd)"
ROOT="\$(dirname "\$SELF_DIR")"
SYSROOT="$MERGED"
INCS="-I\$SYSROOT/usr/include/gtk-2.0 -I\$SYSROOT/usr/include/atk-1.0 -I\$SYSROOT/usr/include/cairo -I\$SYSROOT/usr/include/gdk-pixbuf-2.0 -I\$SYSROOT/usr/include/pango-1.0 -I\$SYSROOT/usr/include/glib-2.0 -I\$SYSROOT/usr/include/pixman-1 -I\$SYSROOT/usr/include/freetype2 -I\$SYSROOT/usr/include/harfbuzz -I\$SYSROOT/usr/include/libpng16 -I\$SYSROOT/usr/include"
LIBS="-L\$SYSROOT/usr/lib -L\$SYSROOT/lib -lgtk-x11-2.0 -lgdk-x11-2.0 -latk-1.0 -lgio-2.0 -lpangoft2-1.0 -lpangocairo-1.0 -lgdk_pixbuf-2.0 -lcairo -lpango-1.0 -lfreetype -lfontconfig -lgobject-2.0 -lgmodule-2.0 -lgthread-2.0 -lglib-2.0 -lX11 -lxcb -lxcb-shm -lxcb-render -lpixman-1 -lXdamage -lXfixes -lresolv -lXinerama -lXau -lXdmcp"
case "\$*" in
  *cflags*) echo "\$INCS";;
  *libs*) echo "\$LIBS";;
  *) echo "pkg-config wrapper: unhandled query: \$*" >&2; exit 1;;
esac
EOF
chmod +x "$ROOT/cross-bin/pkg-config"

# 4. mini vMac config (larm = Linux ARM, xwn = plain X11 backend).
#    -magnify 1 + -mf 2: start at 2x (1024x684) instead of the 1x default
#    (without -magnify, WantInitMagnify=0 and the window renders unscaled).
cd "$ROOT/minivmac-src"
if [ ! -d cfg ]; then
	echo "==> Generating mini vMac config ..."
	cc setup/tool.c -o setup_t
	./setup_t -t larm -mf 2 -magnify 1 -ci 0 -depth 0 -sound 0 > setup.sh < /dev/null
	bash setup.sh
fi

# 5. Patch the generated Makefile for the cross build:
#    - remove vestigial -L/usr/X11R6/lib (Buildroot wrapper rejects it)
#    - link the glibc-2.20 stat shim
echo "==> Patching Makefile ..."
sed -i 's| -L/usr/X11R6/lib||' Makefile
if ! grep -q "compat-glibc.o" Makefile; then
	sed -i 's|^\tbld/PROGMAIN\.o \\$|\tbld/PROGMAIN.o \\\n\tbld/compat-glibc.o \\|' Makefile
	printf '\n\nbld/compat-glibc.o : compat-glibc.c\n\tgcc -c "compat-glibc.c" -o "bld/compat-glibc.o" $(mk_COptions)\n' >> Makefile
fi

# 6. Build everything
echo "==> Building mini vMac ..."
mkdir -p "$ROOT/tools/bin"
for t in gcc strip g++; do ln -sf ../../cross-bin/$t "$ROOT/tools/bin/$t"; done
PATH="$ROOT/tools/bin:$PATH" make -C "$ROOT/minivmac-src" >/dev/null
"$ROOT/cross-bin/strip" "$ROOT/minivmac-src/minivmac"

echo "==> Building wario-companion ..."
"$ROOT/cross-bin/gcc" -O2 "$ROOT/wario-companion/companion.c" \
	-o "$ROOT/wario-companion/wario-companion" \
	$("$ROOT/cross-bin/pkg-config" --cflags gtk+-2.0 x11) \
	$("$ROOT/cross-bin/pkg-config" --libs gtk+-2.0 x11)
"$ROOT/cross-bin/strip" "$ROOT/wario-companion/wario-companion"

echo "==> Building kindle-touch (static musl, no glibc) ..."
if [ ! -x "$ZIG" ]; then
	echo "ERROR: zig not found at $ZIG (needed for static touch build)" >&2
	echo "Download: https://ziglang.org/download/" >&2
	exit 1
fi
"$ZIG" cc -target arm-linux-musleabihf -static -O2 \
	"$ROOT/kindle-touch/touch.c" -o "$ROOT/kindle-touch/touch"

echo
echo "Done. Artifacts:"
for f in minivmac-src/minivmac kindle-touch/touch wario-companion/wario-companion; do
	printf '  %-40s ' "$f"; file -b "$ROOT/$f"
done
echo
echo "Deploy:"
echo "  cp minivmac-src/minivmac kindle-touch/touch wario-companion/wario-companion wario-companion/launch-wario.sh deploy/"
echo "  scp deploy/* pw5:/mnt/us/"
echo "  ssh pw5 'chmod +x /mnt/us/launch-wario.sh && DISPLAY=:0 /mnt/us/launch-wario.sh'"
