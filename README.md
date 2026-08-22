# wario

Fun-Friday e-ink hacking on Ryan's jailbroken Kindle Oasis 1st gen. Named
after the board itself — `/proc/cpuinfo` on the device reports
`Hardware: Freescale i.MX 6SoloLite based Wario Board`.

Started as a "could Bandar Labs' [Cobalt](https://bandarlabs.github.io/Cobalt/)
(a Rust SDK/app-platform for Kobo e-readers) work here in spirit?" question.
Cobalt hard-gates on exact Kobo Clara BW (N365) hardware by design and hooks
Kobo's Nickel launcher — none of that applies to a lab126/KindleOS device, and
nothing here shares code with it. What's genuinely borrowed is just the
*shape* of the idea (a small native app talking to a capability-scoped HAL)
applied to the Oasis's real jailbreak stack (WinterBreak + KUAL + the
on-device Alpine armv7 chroot), documented in the `Kindle Oasis (Jailbroken)`
nelson-wiki page.

## Status: pre-work — ABI verification only

Before sinking time into an SDK, checked the one hard blocker that would sink
the whole idea: does a normal Rust cross-compiled binary even run on this
hardware?

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
  choice. No longer purely theoretical: live-tested 2026-08-22 (see
  `wario-companion` below) and the mini vMac cursor visibly disappeared
  or failed to reappear after trackpad-driven motion, despite the
  underlying event delivery being confirmed correct end-to-end. Strong
  suspect is the EPDC driver's partial-refresh/damage-coalescing logic
  dropping a cursor-sized dirty rect as too small to bother committing a
  waveform update for -- needs investigation with this as the concrete
  reproduction case, not a fresh unknown.
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
- Trackpad press/motion/release fire correctly, compute the right
  relative delta, resolve to mini vMac's actual window ID, and clamp
  correctly against its real dimensions (confirmed via temporary
  instrumentation, since removed).
- Not verified: the moved cursor being visibly redrawn on mini vMac's
  side -- see the sharpened e-ink refresh backlog item above.

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

## Known unrelated issue found along the way

`~/.config/containers/registries.conf` is corrupted (mangled TOML — looks
like a botched edit truncated several lines mid-word across the
`docker-registry.ted.com`, `biggie`, and an S3-comparator ECR entry). It
breaks `podman`/`docker pull` system-wide with a TOML parse error. Not
touched here — routed around it by using a Homebrew cross-toolchain instead
of `cross`'s container-based build. Flagged to Ryan separately; needs a
manual fix since guessing the ECR registry hostname back from the mangled
fragment would be a bad idea.
