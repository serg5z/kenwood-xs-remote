# Raspberry Pi B+ build from Apple Silicon

Target: Raspberry Pi B+ Rev 1.2, ARM1176/ARMv6 with VFP, Raspberry Pi OS
Trixie **32-bit**, glibc 2.41 and libgpiod 2.2.1. Docker Desktop must be running
and able to execute `linux/arm/v6` containers. No Docker installation on the Pi
is required. This workflow builds files only; it never starts an XS8 executable.

## Environment choice

| Approach | Assessment |
|---|---|
| Native ARM32 compiler under Docker/QEMU | Selected. A signed Raspbian Trixie filesystem supplies the compiler, headers and target libraries together. Slower than native ARM64 compilation, but avoids a separately maintained sysroot/toolchain. |
| ARM64-hosted cross-compiler | Potentially faster. Requires an ARMv6 hard-float toolchain and an exact Raspbian sysroot, including C++ and CEC dependencies. Generic `arm-linux-gnueabihf` toolchains/libraries may default to ARMv7; changing application flags alone does not fix those libraries. Not implemented. |
| Generic Debian armhf container | Not suitable as a target sysroot for this ARMv6 Pi. Debian armhf and Raspbian armhf are not interchangeable CPU baselines. |

The [Raspbian FAQ](https://www.raspbian.org/RaspbianFAQ) describes its ARMv6/VFP
hard-float baseline. Raspberry Pi's [Trixie armhf image layer](https://raspberrypi.github.io/rpi-image-gen/layer/raspbian-trixie-armhf.html)
also uses the Raspbian archive. The Dockerfile uses native ARM64 Debian tools
only to download and signature-check a foreign Raspbian filesystem; those
Debian binaries/libraries are not copied into the target. Subsequent package
installation and compilation execute in ARM32 under Docker's emulator.

Build dependencies: `make`, GCC/G++ (C11 and C++17), libc development headers,
`libcec-dev`, `libgpiod-dev`, and pthread/dl support. Validation uses binutils,
`file` and pkg-config. libgpiod3 and its headers are pinned to
`2.2.1-2+rpi1+deb13u1`; the image rejects a libc other than glibc 2.41.
Both target repositories are signature checked. No `trusted=yes` or signature
bypass is used. The initial repository keys are downloaded over HTTPS from
their publishers.
Trixie's verifier rejects the legacy public-key files' SHA1 certifications.
The final environment uses the refreshed Raspbian packaged keyring and the
Raspberry Pi keyring pinned to official image-builder commit
`1f5851b24a6ac25b91edfaed5f2b59e770d0ac9e`, checking both key fingerprints.
This follows the [publisher's key refresh](https://github.com/raspberrypi/rpi-image-gen/issues/171);
signature policy is not relaxed. apt update errors are fatal rather than
silently reusing stale indexes.

## Build

From the repository root:

```sh
make pi-build
```

The script finds Docker on PATH or in the standard Docker Desktop location.
An alternative CLI can be selected with `DOCKER=/path/to/docker`. The first run
downloads/builds the environment; later runs reuse Docker's image layer cache.
Every invocation rebuilds the application objects, preventing reuse of objects
compiled with different flags. Source is mounted read-only and the compilation
container has no network, devices, privileged mode or host-service sockets.
Compilation also sets QEMU's CPU to `arm1176`; this exercises the build tools
and loader at the target CPU baseline rather than QEMU's usual newer default.

Output is **`.build/pi/`**, separate from the existing native `.build/` files:

- `cec-xs-bridge` — original direct-GPIO fallback bridge.
- `cec-xs-client` — CEC socket client, no GPIO ownership.
- `xs8-service` — sole GPIO owner in the service architecture.
- `xs8_send` — standalone diagnostic transmitter; not run by this workflow.
- `validation.txt`, `packages.txt`, `SHA256SUMS`, `build-seconds.txt`.

These are the four existing `make all` targets. Other standalone diagnostic
sources, Python Gateway/REPL and frontend assets are not newly packaged here.
For another output location use `PI_BUILD_OUTPUT=.build/pi-review make pi-build`.
`PI_BUILD_IMAGE` overrides the local image name. Do not use the native `.build/`
directory itself or the repository root as the output directory.

Application flags are `-march=armv6 -mfpu=vfp -mfloat-abi=hard -marm`, with the
existing optimization/warning options and C++17 standard. XS8 timing, GPIO,
RT scheduling and protocol code are unchanged.

## Validation and limits

The build must pass checks for ELF32 ARM, hard-float ABI, ARMv6 CPU attributes
(`v6`, `v6K` or `v6KZ`, compatible with ARM1176),
VFP-register argument passing, and loader `/lib/ld-linux-armhf.so.3`. The report
includes headers, ARM attributes, interpreter, direct `NEEDED` entries,
transitive loader resolution and required symbol versions for all four binaries.
The loader's `--list` operation does not execute application `main()`.
libCEC is also checked explicitly because the CEC loader uses `dlopen`, so it
may be absent from the executable's direct dependency list.
Explicit ARMv7/v8, Thumb-2-only ARMv6T2, newer VFP and SIMD dependency attributes
are rejected. Libraries without ARM attributes remain a limitation: metadata
is not a complete instruction-level proof of ARMv6 compatibility.

`packages.txt` records installed versions, including CEC and C++ runtime
dependencies. The Raspbian repositories are rolling within Trixie; other than
the libgpiod version, packages are not snapshot-pinned. Docker caches a completed
environment, but rebuilding it later can select newer packages or fail if the
pinned libgpiod version is removed. Preserve the validated build image and its
package report if repeatability matters. Do not silently substitute generic
Debian armhf packages. QEMU/container success does not establish real Pi GPIO,
CEC adapter support, kernel scheduling behavior or receiver execution.
Backend libraries loaded dynamically inside libCEC, beyond its recorded
`DT_NEEDED` dependencies, are not exhaustively enumerated by this check.

### Validation recorded on 2026-10-12

Apple Silicon / Docker Desktop: `make pi-build` completed in **51 seconds**
with the environment image cached, including forced recompilation and ELF
validation. Initial environment creation required several minutes of downloads
and bootstrap work; a clean cold-build benchmark was not recorded during
repository/key troubleshooting.

| Check | Result |
|---|---|
| Four `make all` executables | ELF32 ARM, CPU `v6`, hard-float ABI, VFP-register arguments |
| Dynamic interpreter | `/lib/ld-linux-armhf.so.3` |
| glibc | `2.41-12+rpt1+deb13u4` |
| libgpiod runtime and headers | `2.2.1-2+rpi1+deb13u1` |
| libCEC runtime | `7.0.0-1+rpt1`, ARMv6 attributes |
| Dependency resolution | Passed in the target container, including explicit libCEC inspection |

GCC 14 emitted ARM psABI informational notes about parameter passing changes
since GCC 7.1; compilation completed. The final complete native `make test`
passed, including 12 REPL tests and 8 socket tests. An earlier run had a
transient fake-socket startup connection refusal. No generated XS8/CEC
application was run.

## Manual deployment and rollback

These are operator steps, **not performed by `make pi-build`**. First inspect
`validation.txt` and `packages.txt`. On the Pi, compare `getconf GNU_LIBC_VERSION`,
`dpkg --print-architecture` and package versions for `libgpiod3`, `libcec7`,
`libstdc++6` and `libgcc-s1`. Confirm required libraries and symbol versions are
available; do not copy container system libraries over the Pi's libraries.
The tested repository selects libCEC 7 (`libcec7`, `7.0.0-1+rpt1`). A Pi with
only libCEC 6 is not automatically compatible with this build; install the
matching Trixie runtime during a separate approved deployment or use the native
build against its installed development/runtime pair.

Copy to a staging directory without replacing running binaries:

```sh
ssh serg@woodland-pi.local 'mkdir -p ~/kenwood-build-review'
scp .build/pi/cec-xs-bridge .build/pi/cec-xs-client .build/pi/xs8-service \
    .build/pi/xs8_send .build/pi/SHA256SUMS .build/pi/packages.txt \
    .build/pi/validation.txt serg@woodland-pi.local:kenwood-build-review/
ssh serg@woodland-pi.local 'cd ~/kenwood-build-review && sha256sum -c SHA256SUMS'
```

Inspect the staged files with `file`/`readelf` and the target loader's `--list`
before any attended activation. Do not execute `xs8_send` as a build smoke
test, start two GPIO owners, or restart an active volume/CD hold. Preserve the
currently working binaries and effective service options. Any later replacement
must be an explicit attended deployment with physical recovery available;
rollback restores those saved binaries/options rather than clearing a safety
fault automatically. This workflow changes no systemd configuration.

## Native Pi fallback

The existing native path remains:

```sh
sudo apt-get install build-essential libcec-dev libgpiod-dev
make all
make test
```

Check the actual installed libgpiod is 2.x before building. `make all` still
defaults to `.build/`; it does not invoke Docker. Offline tests use fake GPIO
backends and must not be replaced with physical motor experiments.
