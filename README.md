# kindle-cobalt

Fun-Friday exploration: is Bandar Labs' [Cobalt](https://bandarlabs.github.io/Cobalt/)
(a Rust SDK/app-platform for Kobo e-readers) portable "in spirit" to Ryan's
jailbroken Kindle Oasis 1st gen?

Not a fork of Cobalt. Cobalt hard-gates on exact Kobo Clara BW (N365) hardware
by design and hooks Kobo's Nickel launcher — none of that applies to a
lab126/KindleOS device. This repo borrows the *shape* of Cobalt's app model
(declarative screens, capability-gated HAL, signed OTA delivery) and targets
the Oasis's actual jailbreak stack (WinterBreak + KUAL + the on-device Alpine
armv7 chroot), documented in the `Kindle Oasis (Jailbroken)` nelson-wiki page.

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

## Next steps (not started)

- Pick 2-3 of the *Oasis's* real primitives to wrap first: `/dev/fb0`
  framebuffer read (already scripted in `kindle-agent`), `lipc-set-prop`
  power control, `/dev/input/eventN` touch injection.
- Sketch a minimal `OasisApp` trait + `ScreenBuilder` inspired by Cobalt's
  `KoboApp`/`ScreenBuilder`, but backed by the above instead of Kobo's
  `kobo-hal`.
- Punt on the signed-App-Store / OTA-delivery piece entirely for now — no
  reason to build that until there's more than one app.

## Known unrelated issue found along the way

`~/.config/containers/registries.conf` is corrupted (mangled TOML — looks
like a botched edit truncated several lines mid-word across the
`docker-registry.ted.com`, `biggie`, and an S3-comparator ECR entry). It
breaks `podman`/`docker pull` system-wide with a TOML parse error. Not
touched here — routed around it by using a Homebrew cross-toolchain instead
of `cross`'s container-based build. Flagged to Ryan separately; needs a
manual fix since guessing the ECR registry hostname back from the mangled
fragment would be a bad idea.
