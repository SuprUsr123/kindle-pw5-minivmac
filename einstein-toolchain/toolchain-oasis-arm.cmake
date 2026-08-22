# CMake cross-toolchain for the Kindle Oasis 1st-gen (i.MX6 SoloLite,
# Cortex-A9, 32-bit ARM, EABI5, soft-float ABI despite VFP hardware --
# confirmed by readelf on wario-companion showing no Tag_ABI_VFP_args).
#
# Reuses the same Homebrew "arm-unknown-linux-gnueabi" toolchain and
# oasis-sysroot (pulled live from the device) already proven working by
# the wario/mini vMac port. Do NOT use armv7-*-musleabihf toolchains --
# the device's userland is glibc (/lib/ld-linux.so.3), not musl.
#
# WARIO_SYSROOT env var overrides the sysroot location if set.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(_toolchain_bin /opt/homebrew/opt/arm-unknown-linux-gnueabi/bin)
set(CMAKE_C_COMPILER   ${_toolchain_bin}/arm-unknown-linux-gnueabi-gcc)
set(CMAKE_CXX_COMPILER ${_toolchain_bin}/arm-unknown-linux-gnueabi-g++)

if(DEFINED ENV{WARIO_SYSROOT})
  set(CMAKE_SYSROOT $ENV{WARIO_SYSROOT})
else()
  set(CMAKE_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../oasis-sysroot")
endif()

set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# No real .pc files shipped in oasis-sysroot (it's a device-pulled lib/header
# subset, not a full SDK) -- keep pkg-config out of the loop entirely so
# CMake's own Find modules (FindX11 etc.) do the work via find_path/find_library.
set(PKG_CONFIG_EXECUTABLE "PKG_CONFIG_EXECUTABLE-NOTFOUND" CACHE FILEPATH "disabled for cross-compile" FORCE)

# The device's glibc is 2.20 (2014) -- predates getentropy() (glibc 2.25,
# 2017), which the Homebrew toolchain's libstdc++.so unconditionally
# references (std::random_device, baked into every shared build of
# libstdc++ regardless of whether the program uses it). Two-part fix:
#   1. Static-link libstdc++/libgcc so we control exactly which archive
#      members get pulled in, instead of inheriting the prebuilt .so's
#      hard NEEDED entry.
#   2. Still need to satisfy the getentropy symbol itself once anything
#      (even <iostream>'s static init) pulls in libstdc++'s random.o --
#      supply it via a tiny /dev/urandom-backed shim, since the device
#      kernel (Linux 3.0.35-lab126) also predates getrandom(2) (3.17).
# Verified end-to-end on real hardware: a trivial C++ program linked this
# way runs on the Oasis (2026-08-21).
set(_getentropy_shim_src "${CMAKE_CURRENT_LIST_DIR}/compat/getentropy_shim.c")
set(_getentropy_shim_obj "${CMAKE_BINARY_DIR}/getentropy_shim.o")
if(NOT EXISTS ${_getentropy_shim_obj})
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} --sysroot=${CMAKE_SYSROOT} -c ${_getentropy_shim_src} -o ${_getentropy_shim_obj}
    RESULT_VARIABLE _getentropy_shim_rc)
  if(NOT _getentropy_shim_rc EQUAL 0)
    message(FATAL_ERROR "Failed to build getentropy shim (${_getentropy_shim_rc})")
  endif()
endif()

# oasis-sysroot's usr/lib/libpthread.so is a GNU ld GROUP script requiring
# /usr/lib/libpthread_nonshared.a, a build-only glibc artifact that's
# absent both from the sysroot and the live device (confirmed via ssh) --
# it only carries a couple of fork-safety stub symbols, unneeded here.
# cmake/compat/lib/libpthread.so is the same script with that dependency
# dropped; put it first on the link search path so plain -lpthread (as
# used by CMake's FindThreads and everything downstream) resolves to the
# working version. Verified end-to-end on real hardware (2026-08-21).
set(_compat_libdir "${CMAKE_CURRENT_LIST_DIR}/compat/lib")
set(CMAKE_EXE_LINKER_FLAGS "-L${_compat_libdir} -static-libstdc++ -static-libgcc ${_getentropy_shim_obj}" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS "-L${_compat_libdir} -static-libstdc++ -static-libgcc ${_getentropy_shim_obj}" CACHE STRING "" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS "-L${_compat_libdir} -static-libstdc++ -static-libgcc ${_getentropy_shim_obj}" CACHE STRING "" FORCE)

# The device's glibc build for this arm target only exports plain
# fcntl/open/etc (confirmed via nm -D on the real libc.so.6: "fcntl" is
# present, "fcntl64" is not) -- it was never split into LFS (_FILE_OFFSET_BITS=64)
# variants. FLTK's own CMakeLists unconditionally adds
# -D_FILE_OFFSET_BITS=64 (plus _LARGEFILE64_SOURCE/_LARGEFILE_SOURCE) as a
# per-target compile definition, which makes glibc's <fcntl.h> map
# fcntl()->fcntl64() and produces a link-time undefined reference. Target
# defines land earlier on the compiler command line than these global
# flags, so appending -U here removes them again for every translation
# unit, regardless of what an individual target's CMakeLists forces.
# GCC 15's default C standard now treats empty-parens `()` function
# declarations as strictly "takes no arguments" (a C23 change) rather
# than the old K&R "unspecified arguments" -- newt64's NewtVM.c relies
# on the old behavior for its native-function trampoline typedef.
# -std=gnu17 restores it.
set(_libffi_dir "${CMAKE_CURRENT_LIST_DIR}/compat/libffi-arm")
# Einstein's CMakeLists enables -Werror; GCC 15.2 (this toolchain) is
# stricter about some warning classes (e.g. -Wformat-truncation) than
# whatever older GCC Einstein's own CI targets, producing false-positive
# build failures on code that isn't actually broken (buffer sizes here
# are generously oversized, GCC just can't prove it statically). Keep
# the warnings, drop the -Werror so pedantic false positives don't block
# a genuinely working build.
# strlcpy/strlcat: also predate this glibc (added upstream only in
# 2.38, 2023) -- Einstein's FLTK frontend calls strlcpy directly.
# Xrender.h/render.h: present as a shared lib in oasis-sysroot
# (libXrender.so, pulled from the device) but the device never shipped
# the X11 extension HEADERS (dev-only files, no reason for a runtime
# image to have them) -- reuse Homebrew's host-installed copies. These
# are portable protocol/struct definitions with no compiled code, safe
# to mix from the host even when cross-compiling.
set(_xrender_incs "-I/opt/homebrew/opt/libxrender/include -I/opt/homebrew/opt/xorgproto/include")
set(_strlcpy_compat_h "${CMAKE_CURRENT_LIST_DIR}/compat/strlcpy_compat.h")
set(CMAKE_C_FLAGS   "-std=gnu17 -Wno-error -include ${_strlcpy_compat_h} ${_xrender_incs} -I${_libffi_dir}/include -U_FILE_OFFSET_BITS -U_LARGEFILE64_SOURCE -U_LARGEFILE_SOURCE" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "-Wno-error -include ${_strlcpy_compat_h} ${_xrender_incs} -I${_libffi_dir}/include -U_FILE_OFFSET_BITS -U_LARGEFILE64_SOURCE -U_LARGEFILE_SOURCE" CACHE STRING "" FORCE)

set(_strlcpy_shim_src "${CMAKE_CURRENT_LIST_DIR}/compat/strlcpy_shim.c")
set(_strlcpy_shim_obj "${CMAKE_BINARY_DIR}/strlcpy_shim.o")
if(NOT EXISTS ${_strlcpy_shim_obj})
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} --sysroot=${CMAKE_SYSROOT} -c ${_strlcpy_shim_src} -o ${_strlcpy_shim_obj}
    RESULT_VARIABLE _strlcpy_shim_rc)
  if(NOT _strlcpy_shim_rc EQUAL 0)
    message(FATAL_ERROR "Failed to build strlcpy shim (${_strlcpy_shim_rc})")
  endif()
endif()

# Einstein's NativeCalls feature needs libffi, absent from oasis-sysroot
# (it was never part of the device's own userland). Einstein's repo ships
# a prebuilt ARM Linux libffi under libffi-armlinux/, but readelf shows
# it was built with "EABI version 0" (a legacy/OABI-era artifact,
# probably from its original Maemo/OpenZaurus port) -- genuinely
# incompatible with this toolchain's EABI5 target; ld refuses to link it
# ("failed to merge target specific data"). Cross-built libffi 3.4.6
# from source instead (autotools: ./configure --host=arm-unknown-linux-gnueabi
# CC="arm-unknown-linux-gnueabi-gcc --sysroot=...", --disable-shared),
# vendored under compat/libffi-arm/. Verified matching EABI via readelf
# (aeabi attributes present, no version mismatch) (2026-08-22).
# Exposed as a cache var (not folded into CMAKE_EXE_LINKER_FLAGS as
# "-lffi") because CMAKE_EXE_LINKER_FLAGS lands BEFORE the object files
# on the actual link line, and GNU ld only pulls archive members for
# symbols already needed at that point -- an early "-lffi" is silently a
# no-op. Einstein's own CMakeLists.txt links this explicitly via
# target_link_libraries, which CMake correctly places after the objects.
set(EINSTEIN_LIBFFI_ARCHIVE "${_libffi_dir}/lib/libffi.a" CACHE FILEPATH "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS "-L${_compat_libdir} -static-libstdc++ -static-libgcc ${_getentropy_shim_obj} ${_strlcpy_shim_obj}" CACHE STRING "" FORCE)
