# Mini vMac for Kindle Oasis

**A working Mini vMac port running System 6.0.8 natively on a jailbroken
first-generation Kindle Oasis, with direct touchscreen input, an on-screen
keyboard and trackpad, and MacPaint and MacWrite attached at launch.**

The emulator runs directly on stock KindleOS—no chroot at runtime—and renders
a 2x-scaled Mac Plus display onto the Oasis's unusual 8-bit StaticGray X11
screen. A separate GTK2 companion window docks beneath the emulated display
and supplies the controls that make classic Mac software usable on a
touchscreen.

`wario` is Amazon/lab126's codename for this Kindle hardware board, not a
reference to the Nintendo character: `/proc/cpuinfo` reports
`Hardware: Freescale i.MX 6SoloLite based Wario Board`.

## What works

- System 6.0.8 boots and runs at 2x scale on the real e-ink panel.
- MacPaint and MacWrite floppy images mount automatically alongside the
  startup disk.
- Direct screen touches support pen-style clicking and dragging, with a
  toggleable cursor-nudge mode.
- The docked companion provides a QWERTY keyboard, relative trackpad,
  tap-to-click, double-click, triple-click, and explicit mouse-down/up.
- Mini vMac continues running while the companion owns X input focus.
- Native hard-float Rust and glibc C binaries have both been verified on the
  device.

This is an owner-operated hardware-hacking prototype, verified end to end on
an Oasis 1st gen. It is not yet packaged as a KUAL extension, and the build
still depends on a locally extracted Oasis sysroot and generated Mini vMac
configuration.

## Launching on the Kindle

The standard launcher expects these files in `/mnt/us`:

```text
minivmac
wario-companion
system608.img
macpaint.img
macwrite.dsk
launch-wario.sh
```

Deploy the launcher, then run it over SSH:

```sh
scp wario-companion/launch-wario.sh kindle-ts:/mnt/us/
ssh kindle-ts 'chmod +x /mnt/us/launch-wario.sh && /mnt/us/launch-wario.sh'
```

The launcher sets `DISPLAY=:0`, starts Mini vMac with all three disks, and
docks the companion underneath it. ROM and disk images are not included in
this repository.

## Repository layout

- `minivmac-src/` — Mini vMac 36.04 plus the Kindle X11/input adaptations.
- `wario-companion/` — GTK2 keyboard, trackpad, pen-mode engine, and launcher.
- `kindle-touch/` — small native evdev touch-injection utility.
- `cross-bin/` — wrappers for the Oasis glibc cross-toolchain and sysroot.
- `abi-probe/`, `hello-c/` — the original ABI verification programs.
- `oasis-hello/`, `wario-sdk/` — the earlier native Rust rendering experiment.

## Development notebook

This began as a two-day Fun-Friday investigation into whether the Oasis could
support native applications in the spirit of Bandar Labs'
[Cobalt](https://bandarlabs.github.io/Cobalt/) for Kobo readers. Cobalt itself
is Kobo-specific and no code is shared; the useful inspiration was the shape
of a small native application over a hardware-specific layer.

The notes below preserve the actual investigation: hypotheses, dead ends,
on-device measurements, fixes, and verification.

### ABI verification

Before sinking time into an SDK, the first question was whether a normal Rust
cross-compiled binary could run on this hardware at all.

### Finding: yes. Standard `armv7-unknown-linux-musleabihf` (hardfloat) works.

- Device: Kindle Oasis 1st gen, `armv7l`, Freescale i.MX 6SoloLite ("Wario"),
  kernel `3.0.35-lab126`. `/proc/cpuinfo` shows `vfp vfpv3 neon` — real FPU
  hardware present.
- The device's own system binaries (checked `/usr/bin/lipc-set-prop`) declare
  **`EF_ARM_ABI_FLOAT_SOFT`** in their ELF header (EABI5, e_flags
  `0x05000202`) — lab126 built their userland soft-float despite the FPU
  being there in hardware.
- This raised the question of whether Rust's default hardfloat target
  (`armv7-unknown-linux-musleabihf`) would even execute. It does: a fully
  static musl binary doesn't link against any softfloat system libs, so the
  userland ABI mismatch is irrelevant — only the kernel's VFP context-switch
  support matters, and that's present.
- Verified empirically, not just in theory: cross-compiled `abi-probe/` (a
  trivial hello-world that round-trips an `f64` through `println!`) for
  `armv7-unknown-linux-musleabihf` using the
  `messense/macos-cross-toolchains` Homebrew tap, copied it to the device
  over `kindle-ts` (Tailscale SSH), and ran it **directly on stock KindleOS,
  no chroot**:

  ```
  ELF 32-bit LSB executable, ARM, EABI5 version 1 (SYSV), statically linked
  EABI version: 5, ABI_FLOAT_HARD: True

  $ /mnt/us/abi-probe-hf
  hello from kindle-cobalt abi-probe, args=1
  exit=0
  ```

  Correct float formatting, clean exit. No softfloat cross target needed.

## oasis-hello — first real app

A tiny native Kindle app, not just a probe. Hand-rolled 5x7 bitmap font
(no TTF, no font licensing to think about) rendered into a full-panel PNG
via the `image` crate (pure Rust, no C deps — cross-compiled clean on the
first try), then displayed with the device's own `eips` utility
(`eips -c` to clear, `eips -g <png> -f` for a full e-ink refresh).

Runs natively on the Oasis, no chroot. Verified with a live
`kindle-screenshot` capture of the actual panel, not just clean exit codes.

```sh
export PATH="$HOME/.rustup/toolchains/nightly-aarch64-apple-darwin/bin:$PATH"
cd oasis-hello && cargo build --release --target armv7-unknown-linux-musleabihf
scp target/armv7-unknown-linux-musleabihf/release/oasis-hello kindle-ts:/mnt/us/
ssh kindle-ts '/mnt/us/oasis-hello /mnt/us/oasis-hello.png'
```

## Build setup

```sh
brew tap messense/macos-cross-toolchains
brew install messense/macos-cross-toolchains/armv7-unknown-linux-musleabihf
rustup target add armv7-unknown-linux-musleabihf   # under the rustup-managed
                                                    # toolchain, not the
                                                    # Homebrew rustc that's
                                                    # first on PATH — see
                                                    # abi-probe/.cargo/config.toml
```

`abi-probe/.cargo/config.toml` pins the linker to the Homebrew-tap gcc. Build
with the rustup toolchain explicitly ahead of Homebrew's on `PATH`:

```sh
export PATH="$HOME/.rustup/toolchains/nightly-aarch64-apple-darwin/bin:$PATH"
cd abi-probe && cargo build --release --target armv7-unknown-linux-musleabihf
```

Deploy: `scp target/armv7-unknown-linux-musleabihf/release/abi-probe kindle-ts:/mnt/us/` — runs directly, no chroot needed.

## Feature requests / next steps (2026-08-21)

Backlog as of the mini vMac + kindle-agent milestone (real System 6.0.8
booting at 2x on real hardware, native touch injection working end to
end). Not prioritized against each other yet.

**Input & interaction**
- Tune touch UX with mini vMac's own knobs: `-dct 12` (slower double-click
  window), `-emm 0` + `-fullscreen 1` (tablet-friendly mouse emulation) --
  cheap to try, from live diagnosis of rough click/drag behavior
- `kindle-agent` REPL mode (interactive loop: `click x y`, `clickAndDrag
  x1 y1 x2 y2`, `doubletap x y` -- primitives already exist via
  `kindle-touch`, just needs a loop instead of one-shot CLI)
- `powerOn`/`powerOff` REPL commands -- needs a decision: screen
  sleep/wake (`preventScreenSaver`) vs. actual `poweroff`/reboot
  (different risk profile; a reboot wipes screensaver/resleep-timer
  state, confirmed live this session)
- ~~On-screen keyboard (kterm reuse, separate-process architecture)~~ --
  done, but built as an original standalone app instead of reusing kterm.
  See `wario-companion` below.

**Mac-side usability**
- Silence the ALSA "cannot open audio device" spam in the logs -- either
  wire up real sound or just build with `-sound 0`
- A real bootable hard-disk image instead of a floppy each launch
  (persistence across sessions, no "insert disk" step)
- Verify the floppy image isn't getting corrupted by repeated writes --
  confirm whether mini vMac writes back to `system608.img` in place or
  needs a working copy

**Operational polish**
- A KUAL launcher entry so mini vMac starts from the Kindle's own menu,
  not SSH -- matches how kterm/every other app on this device actually
  gets used
- A one-shot launch script that handles `preventScreenSaver`, ROM/disk
  paths, and cleanup automatically (currently all manual steps done by
  hand each session)
- Battery-aware behavior -- e.g. warn or auto-quit below some threshold,
  given the device has already gone to sleep mid-session from power draw
  once this session

**Bigger/deferred**
- E-ink refresh quality pass -- dirty-rect tuning, ghosting, waveform
  choice. Still a real, open item, but the 2026-08-22 "cursor doesn't
  move" symptom below was **not** this -- that was a genuine emulation
  bug (mini vMac pausing when unfocused), now fixed. See
  `wario-companion` below for the real cause and fix.
- Xephyr investigation -- explicitly deferred as "not now" during
  planning; still on the table if a second/third app makes the
  generic-abstraction payoff worth it (nested X server presenting a sane
  visual to guests, so no app has to know about the Kindle's unusual
  8-bit StaticGray display)
- More classic Mac software to try beyond System 6.0.8 (games, apps) --
  pure fun factor

## wario-companion: keyboard/trackpad window (2026-08-22)

A separate, original GTK2 app (`wario-companion/companion.c`, MIT
license) providing an on-screen QWERTY keyboard and a relative-motion
trackpad, in one window with one mode-toggle button. Not a kterm/
matchbox-keyboard reuse -- see the file's own header comment for the
GPL-compatibility reasoning. Delivers input to mini vMac via
`XSendEvent` aimed directly at its window (found by title substring
match), not XTest -- mini vMac's event loop checks `event->window`
explicitly and never inspects `send_event`, so a correctly-addressed
synthetic event is indistinguishable from a real one to it.

### The real bug: not focus, not Z-order, not injection method

Companion's window was invisible on launch. Chased this down several
wrong paths before finding the real cause in `/var/log/messages` on the
device itself:

1. Tried `XRaiseWindow` -- silently ignored.
2. Tried the standard EWMH `_NET_ACTIVE_WINDOW` ClientMessage, mirroring
   what mini vMac's own (dormant, drag-drop-only) `MyActivateWind` does
   -- also silently ignored.
3. Read `/etc/xdg/awesome/lab126_button_handling.lua` and
   `lab126LayerLogic.lua`: lab126's WM grants focus/Z-order only via
   `setFocusedClient(c)`, called from a real `Button1Down` event under
   the tap point. Tried a synthetic self-tap via `XTestFakeButtonEvent`
   -- added a diagnostic `button-press-event` handler on the window
   itself and confirmed the event **never arrived at any X client**,
   despite every XTest call reporting protocol-level success. This
   device's real touch path is the XInput2 `multitouch` driver over
   evdev, not the legacy core pointer XTest targets.
4. Switched the self-tap to `kindle-touch` (this project's own evdev
   injection tool, proven all session for mini vMac itself) -- still no
   visible change.
5. Read `lab126_application_layer.lua`'s `applicationLayer_layout`:
   **every newly-added `N:application` window is already auto-focused**
   (`if action == "added" then setFocusedClient(updatedWindow.c) end`)
   -- no self-tap should ever have been necessary in the first place.
6. Checked `/var/log/messages` directly and found the real answer:
   `WindowManager:bad-client-name ... window does not conform to winmgr
   naming convention - leaving hidden`. The window was never unfocused
   or mis-stacked -- it was **hidden by the WM the entire time**.

Root cause: the title format uses `_` as the field separator
(`L:A_N:application_ID:..._M:false_...`), and the app ID value
(`net.gryphel.wario_companion`) contained its own underscore, corrupting
the WM's field parser. mini vMac's ID (`net.gryphel.minivmac`) has none,
which is why it never hit this. Fixed by dropping the underscore
(`wariocompanion`) -- the window appeared immediately, auto-focused, with
zero raise/activate/self-tap code needed. All four other mechanisms
tried were real dead ends, not partial progress; removed entirely rather
than left in as defensive dead code.

### Verified on real hardware

- Companion window appears on top and focused on launch (screenshot).
- Mode toggle switches between keyboard and trackpad panels (screenshot).
- Trackpad drag-select and clicks now genuinely work end to end -- see
  "Trackpad actually did nothing" below for the real bug and fix.

### Trackpad actually did nothing -- mini vMac pausing itself, not a delivery bug (2026-08-22)

After docking, Ryan reported the trackpad "doesn't seem to actually work
yet." Easy to misdiagnose as e-ink refresh dropping small updates (the
backlog item above already primed that theory) -- that was wrong.

Instrumented every layer in turn rather than guess:

1. Companion's own `on_pad_button_press`/`on_pad_motion`/`on_click1`:
   all fired correctly, with correct deltas.
2. `send_button`/`send_motion`: resolved mini vMac's real window ID
   (`0x1c00004`) and correct target coordinates every time; `XSendEvent`
   returned success for every call.
3. Given the sender side was fully clean, instrumented mini vMac's own
   `ButtonPress`/`ButtonRelease`/`MotionNotify` cases directly (it's our
   own fork) and reran the exact same test: **every single event was
   received, with exactly the right coordinates and button state.**
   Delivery was never the problem, at any layer.

The real cause was two steps further in: mini vMac's own main loop
pauses its entire 68k CPU emulation when it loses real X input focus
(`gBackgroundFlag`, driven by `FocusOut`) unless `RunInBackground` is
on, which defaults off. `wario-companion`'s window legitimately holds
real X focus for keyboard/trackpad input while mini vMac sits unfocused
in the background sharing the screen -- exactly the situation this
flag exists for, and exactly the situation a normal single-window mini
vMac session never hits. Our events were landing and correctly updating
`MousePositionNotify`/`MyMouseButtonSet` state the entire time; the CPU
just never advanced to act on any of it.

Fixed in `src/INTLCHAR.h` (tracked source), not `cfg/CNFGRAPI.h` (a
gitignored, regenerated file that a fresh `setup.sh` run would silently
revert). Verified live: dragging onto the System Startup and Trash
icons and clicking now correctly shows each one selected (inverted icon
and label), cursor tracking the drag the whole way.

### Docked at the bottom, not full-screen (2026-08-22, same day)

Ryan caught that the companion covered the whole screen, hiding mini
vMac entirely instead of sharing it -- a real usability gap, not
cosmetic. Root cause: the `L:A_N:application` tag (copied from mini
vMac) puts a window in the WM's app layer, whose own layout function
(`lab126_application_layer.lua`'s `prv_position_application`) forcibly
resizes every window in that layer to fill the whole available area --
our own requested size was always being overridden, silently.

Fix: retagged as `L:KB_N:keyboard`, a different, real WM layer
(`lab126_keyboard_layer.lua`, used by the OS's own on-screen keyboards)
built for exactly this. `prv_position_keyboard` anchors the window to
the bottom of the screen at full width but preserves whatever height
the window already has when first managed -- no forced full-screen.
mini vMac's window is a fixed 1024x684 on a 1072x1448 panel, leaving
~764px already empty below it; sized the companion to dock in exactly
that space. Verified live: mini vMac's full desktop (menu bar, icons)
stays visible above, companion's keyboard sits below with no overlap,
no `bad-client-name` warnings in `/var/log/messages` for the new tag.

### MacPaint and MacWrite attached by default (2026-08-22)

Confirmed first: this build emulates a **Mac Plus** (`CurEmMd kEmMd_Plus`
in `setup.sh`, also consistent with `EmADB 0` -- the Plus predates ADB).

Ryan's `~/Downloads/MacPaint.img` (402,432 bytes, real HFS floppy,
volume name "MacPaint") and `MacWrite 4.5.dsk` (Apple DiskCopy 4.2
format, 409,600-byte payload) both mount directly with zero conversion.
Confirmed via source before touching hardware:
`Sony_SupportDC42 1` is enabled in `setup.sh`, and `SONYEMDV.c` has
genuine auto-detection logic (checks the DC42 magic word at header
offset 82, then skips the 84-byte header transparently) -- not just
unused constants.

mini vMac's own `Sony_InsertIth` loop (`OSGLUXWN.c`) accepts every
command-line argument after the ROM as a disk to insert, up to
`NumDrives 6`, stopping only on the first one that fails to open. So
attaching more floppies is just appending paths:
`minivmac system608.img macpaint.img macwrite.dsk`.

Deployed both images to `/mnt/us/macpaint.img` and `/mnt/us/macwrite.dsk`
and added `wario-companion/launch-wario.sh` as the one standard way to
boot the whole setup (mini vMac with all three disks + wario-companion
docked below), rather than leaving "attach these by default" as
something to remember to type by hand each session. Verified live: all
three floppy icons ("System Startup", "MacPaint", "MacWrite") appear on
the desktop from a single script invocation.

(A local test on minnie's own Homebrew-installed Mini vMac.app was
attempted first for a fast sanity check, but that's a different,
separately-configured build -- its GUI expects the ROM dragged in by
hand rather than taking it as a CLI arg, which is why it hung waiting
for input rather than confirming anything. Not representative of our
own build's config; the real verification is the on-device test above.)

## Investigated and shelved: Einstein (Newton emulator) port (2026-08-21)

Ryan asked whether [pguyot/Einstein](https://github.com/pguyot/Einstein) (the
open-source Apple Newton/NewtonOS 2.1 emulator) could run on the Oasis the
same way mini vMac does. He already has a working copy on minnie at
`~/devel/einstein-newton-emu/` (prebuilt `Einstein.app`, not built from
source there) plus both ROMs it needs (`MessagePad 2100 (717006).rom`,
8MB), and it was live-running on minnie during this check
(`Einstein -geometry 480x680+80+80`).

**Verdict: low feasibility for usable interactive emulation on this
hardware.** Not attempted — a feasibility read before sinking build time in,
same discipline as the original wario ABI check.

### CPU: the real blocker

- Oasis host: single-core Freescale i.MX6 SoloLite, Cortex-A9, ~1GHz-class
  (`BogoMIPS 1987.37`, confirmed via `/proc/cpuinfo` on `kindle-ts`).
- Einstein's own wiki (Emulator Architecture page) describes its "JIT" as a
  two-step translation: Newton-ARM instructions are compiled once into an
  *internal bytecode*, which is then interpreted/dispatched on every
  execution, with a page-cache of translated bytecode. This is not a true
  dynamic recompiler emitting host-native machine code — it's a cached
  bytecode interpreter. That matters specifically here because it means
  **same-ISA (ARM host emulating ARM guest) buys nothing**: the
  interpretation loop runs on the host regardless of instruction-set match,
  so raw host clock×IPC is what governs speed, not architecture overlap.
- The project's own documentation states plainly that it is "awfully slow
  on ARM PDAs" — devices generally comparable to or faster than a single
  Cortex-A9 at ~1GHz. The Oasis's CPU is weaker than the hardware that
  earned that complaint.
- Guest target (`MessagePad 2100` ROM) is a StrongARM SA-1100 @ 162MHz
  running full NewtonOS 2.1 (Toolbox, NewtonScript VM, handwriting
  recognition, GC) — a much heavier guest workload than mini vMac's
  68000/System 6.0.8 target, which is a large part of why mini vMac is
  viable on weak/embedded hardware and Einstein is a materially different
  bet.

### Memory: checked, corrected, not actually the constraint

First pass over-read `/proc/meminfo`'s raw `MemFree: 20928kB` as "21MB
free" and flagged it as risky. That was wrong — `Buffers` (28MB) +
`Cached` (160MB) is reclaimable page cache, so real available headroom is
closer to **~209MB**, not 21MB. Confirmed live: mini vMac itself was
running on-device throughout this check (PID 17260) at only **6.5MB RSS**
out of 512MB total — proof this device handles a lightweight emulator
fine and RAM isn't what would sink a port. Retracted the RAM objection;
the CPU-architecture argument above stands on its own.

### Other real cost if attempted

- No existing armv7/oasis-sysroot build of Einstein exists — this would be
  a from-scratch cross-compile port, not "deploy the existing binary."
- Linux builds of Einstein use FLTK (per the 2024.4.21 changelog), not raw
  Xlib. FLTK's own X11 backend would very plausibly hit the same class of
  8-bit-StaticGray depth/visual mismatch this session spent real effort
  diagnosing and hand-patching in mini vMac's raw Xlib code (see the
  `minivmac-src` git history) — except FLTK is a much heavier abstraction,
  harder to surgically patch than the ~15 call sites fixed there.

### Not measured

Didn't benchmark actual Newton-boot wall-clock time under Einstein, on
minnie or otherwise, to turn "awfully slow" into a real number scaled to
the Oasis's CPU. The verdict above is an evidence-grounded inference from
the project's documented architecture and Ryan's live device specs, not a
direct timing measurement. If this ever gets revisited, that's the first
thing to actually do rather than reason about further.

### Update (2026-08-22): actually attempted, builds and runs, blocked on visual/depth mismatch

Ryan asked to actually try the cross-compile rather than stop at the
feasibility read. Full writeup, toolchain, and patches are in
`~/devel/einstein-newton-emu/` (separate git repos: the top-level dir
for the cross-compile toolchain, `src/` for the Einstein fork itself —
see both commit messages for complete detail). Summary:

- **Built clean**: FLTK 1.4.4, newt64, a from-source cross-build of
  libffi 3.4.6 (Einstein's own bundled `libffi-armlinux/` turned out to
  be built for an incompatible EABI version), and Einstein itself, all
  targeting arm-unknown-linux-gnueabi/EABI5 against oasis-sysroot.
- **Genuinely new territory vs. the wario port**: this is Einstein's
  first C++ build on this toolchain/sysroot combination (wario is all
  C). Surfaced a real ~10-year gap between the Homebrew cross
  toolchain's assumptions (GCC 15.2, glibc ≥2.25/2.38-era libstdc++ and
  libc) and the device's actual userland (glibc 2.20 from 2014, kernel
  3.0.35-lab126) — getentropy, strlcpy/strlcat, `fcntl64`, and a broken
  `libpthread.so` linker script all needed compat shims. Every shim
  that could be independently checked was verified running on real
  hardware before being folded into the toolchain.
- **Deploys and runs**: the ARM binary launches on the actual Oasis,
  creates windows, and draws into them (confirmed via `xwininfo`, not
  assumed).
- **Doesn't render visibly — root-caused, not a mystery**: this
  session's own predicted risk (FLTK's X11 backend hitting the same
  class of depth/visual mismatch mini vMac needed hand-patching for)
  turned out real, just not where expected. The root window is
  confirmed `Depth 8, StaticGray` (matches the e-ink hardware) via
  `xwininfo -root`. A simple hand-written FLTK test window (no images)
  rendered *correctly* — genuinely refuting part of the original
  worry. But Einstein's own Settings panel came up `Depth 32,
  TrueColor` — FLTK silently upgrades to a compositing-capable visual
  for some windows (likely triggered by an icon/image widget), and the
  lab126 launcher's custom WM won't composite a window whose visual
  doesn't match the root's, even with the correct `L:A_N:application_...`
  WM_NAME tag (which does work — confirmed via `xwininfo`, and is what
  makes mini vMac and a plain FLTK window visible at all). Matches the
  `X_PolyFillRectangle`/`X_PolyText16` "BadMatch" protocol errors seen
  in the log.
- **Not fixed this session**: needs either forcing FLTK to the root's
  8-bit StaticGray visual for every window (`Fl::visual()` or similar),
  or finding and removing whatever in `TFLSettingsUI.fl` triggers the
  visual upgrade. CPU-speed concern from the original feasibility read
  is now moot until rendering works at all — never got far enough to
  measure it.

### Update 2 (2026-08-22): fixed — it boots real Newton OS 2.1, and it's slow

The visual/depth block above turned out simpler than the write-up made
it sound. Ryan pointed at prior Kindle Scribe research (the
`xephyr-visual` project's `swatch3.c`/xeyes retitle work) establishing
that this class of lab126 hardware expects plain apps to stay on the
default depth-8 StaticGray visual. Checked Einstein's own startup code
and found `TFLApp::InitFLTK()` unconditionally called
`Fl::visual(FL_RGB)` — a process-wide override forcing every window
onto 32-bit TrueColor. Removed it (guarded to Linux only). Confirmed
via `xwininfo` that the window now renders at `Depth 8`, and it
displays correctly — no FLTK-level patch needed, just stop overriding
the default.

Also fixed a separate ROM auto-detect issue (Einstein looks for a file
named exactly `717006`, matching the macOS app bundle's internal
naming convention, not `717006.rom`) and clicked through Settings →
Start. **It booted real, unmodified Newton OS 2.1**: boot splash, then
the standard first-run flow (country select, mailing address entry
with the full on-screen keyboard), all rendered correctly on the
physical e-ink screen.

CPU-speed concern from the original feasibility read: confirmed with a
real number instead of speculation. Tapped "Continue" on the
country-select screen and bracketed the response with timestamped
screenshots — still on the country list 10 seconds after the tap,
already on the next screen 17 seconds after. **Roughly 10-15 seconds
per screen transition.** Genuinely too slow for real interactive use;
not yet isolated how much is e-ink partial-refresh overhead (mini
vMac needed real tuning here) versus actual emulated CPU time.

Full write-up with screenshots: `~/devel/einstein-newton-emu/developer-review.html`.

## Known unrelated issue found along the way

`~/.config/containers/registries.conf` is corrupted (mangled TOML — looks
like a botched edit truncated several lines mid-word across the
`docker-registry.ted.com`, `biggie`, and an S3-comparator ECR entry). It
breaks `podman`/`docker pull` system-wide with a TOML parse error. Not
touched here — routed around it by using a Homebrew cross-toolchain instead
of `cross`'s container-based build. Flagged to Ryan separately; needs a
manual fix since guessing the ECR registry hostname back from the mangled
fragment would be a bad idea.
