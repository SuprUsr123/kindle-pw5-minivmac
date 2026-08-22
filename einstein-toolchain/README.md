# einstein-toolchain

CMake cross-toolchain for building [pguyot/Einstein](https://github.com/pguyot/Einstein)
(the open-source Apple Newton emulator) for the Kindle Oasis. Lives here
because it reuses this repo's `../oasis-sysroot` and `../cross-bin`
toolchain wholesale — same device, same Homebrew
`arm-unknown-linux-gnueabi` cross-compiler, same soft-float/EABI5 target
as mini vMac/wario-companion.

**The patched Einstein source itself is NOT here** — it's a full fork
of upstream, kept as its own git repo: `ryan/einstein-oasis-fork` on
Gitea (`http://biggie:3000/ryan/einstein-oasis-fork.git`). This
directory is just the toolchain + compat shims that source needs to
build; clone the fork separately to actually build it.

## Status (2026-08-22)

Builds clean, deploys to the real device, and **boots genuine Newton OS
2.1** — confirmed on physical hardware, not just "should work." Slow in
practice (~10-15s per screen transition; not yet isolated how much is
e-ink partial-refresh overhead vs. actual emulated CPU time). Full
write-up with screenshots: see `developer-review.html` in
`~/devel/einstein-newton-emu/` (the original project directory before
this toolchain was copied here), or the commit history on
`einstein-oasis-fork`.

## Usage

```bash
git clone http://biggie:3000/ryan/einstein-oasis-fork.git einstein-src
cd einstein-src

# Build a native (host) fluid first -- fluid gets cross-compiled to an
# ARM binary that can't run on the host during the build, but Einstein's
# .fl files need a host-runnable fluid to generate code at build time.
cmake -S fltk-src-or-fetched -B build-native-fluid \
  -D FLTK_BUILD_TEST=Off -D FLTK_BUILD_EXAMPLES=Off -D FLTK_BUILD_GL=Off
cmake --build build-native-fluid --target fluid

# Then the actual cross-compile:
cmake -S . -B build-arm \
  -D CMAKE_TOOLCHAIN_FILE=/Volumes/T9/ryan-homedir/devel/wario/einstein-toolchain/toolchain-oasis-arm.cmake \
  -D EINSTEIN_NATIVE_FLUID=/path/to/build-native-fluid/bin/fluid.app/Contents/MacOS/fluid \
  -D EINSTEIN_NO_PULSEAUDIO=ON \
  -D CMAKE_BUILD_TYPE=Release
cmake --build build-arm --target Einstein -j4
```

`EINSTEIN_NATIVE_FLUID` and `EINSTEIN_NO_PULSEAUDIO` are options added
directly to the fork's `CMakeLists.txt` (not stock upstream Einstein) —
see that repo's commit history for why.

## What's in `compat/` and why

The Homebrew `arm-unknown-linux-gnueabi` cross-toolchain (GCC 15.2)
assumes a glibc newer than the Oasis actually runs (glibc 2.20, from
2014, on a Linux 3.0.35-lab126 kernel) — roughly a decade of drift.
Every shim here was independently verified running on real hardware
before being folded in, not just assumed to work:

| File | Why |
|---|---|
| `getentropy_shim.c` | `getentropy()` (glibc 2.25+) is referenced unconditionally by the toolchain's libstdc++ (`std::random_device`), pulled in by anything using `<iostream>`. Falls back to `/dev/urandom`. |
| `strlcpy_compat.h` / `strlcpy_shim.c` | `strlcpy`/`strlcat` (glibc 2.38+) don't exist either; Einstein's FLTK frontend calls `strlcpy` directly. Header is force-included (`-include`) on every translation unit since C++ has no implicit declarations. |
| `compat/lib/libpthread.so` | The sysroot's own `libpthread.so` is a GNU ld `GROUP` script requiring `libpthread_nonshared.a`, a build-only glibc artifact present on neither the sysroot nor the live device. This is the same script with that dependency dropped. |
| `compat/libffi-arm/` | Einstein's own bundled `libffi-armlinux/` was built for an incompatible (pre-EABI5, likely decade-old Maemo/OpenZaurus-era) EABI version — `ld` refuses to link it ("failed to merge target specific data"). This is libffi 3.4.6 cross-built from source instead (`./configure --host=arm-unknown-linux-gnueabi CC="arm-unknown-linux-gnueabi-gcc --sysroot=..." --disable-shared`). |

Also handled directly in the toolchain file (no separate compat file
needed): `-U_FILE_OFFSET_BITS` etc. (device glibc never split
`fcntl`/`fcntl64`, but FLTK's CMakeLists forces `_FILE_OFFSET_BITS=64`
unconditionally), `-std=gnu17` (GCC 15's C23-mode empty-parens
semantics break newt64's native-function trampoline typedef),
`-Wno-error` (GCC 15 is stricter about some warning classes than
whatever compiler upstream CI targets), and Homebrew's
`libxrender`/`xorgproto` header paths (the device ships `libXrender.so`
but never the X11 extension dev headers).

## The two bugs worth knowing about if you touch Einstein's UI code

1. **Window invisible despite drawing correctly**: the custom lab126
   launcher only composites windows whose `WM_NAME` matches
   `L:A_N:application_ID:..._M:false_PC:N_RC:true_O:U`, and only if set
   *before* the window is mapped (same discovery as mini vMac's own
   port — see `../minivmac-src/src/OSGLUXWN.c`).
2. **Window tagged correctly but still invisible**: check the visual
   depth. The Oasis's root display is `Depth 8, StaticGray` (confirmed
   via `xwininfo -root`); the lab126 launcher won't composite a window
   whose visual doesn't match. Einstein's own `TFLApp::InitFLTK()`
   used to unconditionally call `Fl::visual(FL_RGB)`, forcing every
   window onto 32-bit TrueColor — already removed in the fork (guarded
   to `!TARGET_OS_LINUX`), but worth knowing if a future upstream merge
   reintroduces it.
