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
- On-screen keyboard (kterm reuse, separate-process architecture --
  mini vMac is GPLv2-only, kterm is GPLv3, incompatible to combine into
  one binary, so this has to be two processes talking over X11) -- the
  one still-unstarted phase from the original plan

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
  choice, once there's an actual interactive session to judge it against
- Xephyr investigation -- explicitly deferred as "not now" during
  planning; still on the table if a second/third app makes the
  generic-abstraction payoff worth it (nested X server presenting a sane
  visual to guests, so no app has to know about the Kindle's unusual
  8-bit StaticGray display)
- More classic Mac software to try beyond System 6.0.8 (games, apps) --
  pure fun factor

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
