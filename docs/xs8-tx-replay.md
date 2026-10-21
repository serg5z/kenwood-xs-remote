# TX-only disconnected-fixture XS8 replay

`xs8_tx_replay` is a one-shot diagnostic, not a receiver-control frontend.
**The Kenwood must be physically disconnected from XS8.** H11 may be powered.
No process here starts/stops services, launches capture, reads RX, decodes frames,
verifies waveforms or retries transmission. There is no arbitrary-byte or repeat
option. Keep production binaries, units and historical research unchanged.

## Sequence and timing evidence

All bytes are current logical TX:

| Direction | Four frames, exactly once |
|---|---|
| DOWN (default) | `0x17 → 0xE9 → 0x09 → 0x07` |
| UP (explicit) | `0x17 → 0xF9 → 0x09 → 0x07` |

These come from `VolumeControl::start()` / `finish()` in
[volume_control.cpp](../volume_control.cpp) and the
[volume-step API](volume-step-api.md). Context entry is included. No motor is
connected in this experiment. The tool aborts remaining bytes on error or handled
interruption; it does not inject compensating STOP/ROOT frames.

All three **additional** inter-frame delays default to zero. This follows the
immediate progression of `VolumeControl::step()` as an **initial control
experiment only**, not a reproduction of the historical failure's timing.
The incident's exact direction/delays were not supplied from the Pi logs.
The prior 500 ms max-hold setting is not an exact START-to-STOP delay; the older
CLI's 150 ms volume probe and 1000 ms response window are different experiments.

Options (each delay 0–5000 ms):

```text
--direction down|up
--entry-start-ms N
--start-stop-ms N
--stop-root-ms N
--receiver-disconnected    mandatory for real TX
--dry-run                  no GPIO requests or service queries
```

A delay is a minimum from the previous `xs8_gpio_send()` **return/completion
sample** to the next send invocation. Each send then releases TX LOW and waits
a fixed 500 us settling interval. In this explicit TX-only mode there is **no
BUSY sensing**: settling must not be described as a successful BUSY-free check.
Scheduling, GPIO calls, existing transmitter logging and RT restoration add
latency. There is no receiver feedback. Four finite frames and at most 15 s of
configured delays are allowed; Linux scheduling can exceed nominal durations.

## GPIO ownership and production isolation

The new diagnostic API `xs8_gpio_open_tx_only()` exclusively requests **GPIO6
DATA_TX and GPIO19 BUSY_TX**, initially LOW. It never requests GPIO13/26.
`xs8_gpio_send()` retains its existing waveform implementation/constants and
opt-in XS8_RT_PRIORITY behavior. The production `xs8_gpio_open(bus, capture)`
path always sets TX-only mode false and retains its RX request and BUSY checks.
An ordinary bus lacking an RX request is still rejected. No production caller
selects the diagnostic API.

Before opening GPIO the tool queries both exact units, with a two-second timeout
per query:

- `kenwood-xs8.service`
- `kenwood-cec-xs-client.service`

Both must report LoadState=loaded and ActiveState=inactive. Active, failed,
activating, missing, malformed, inaccessible or indeterminate results refuse TX.
No service operation is attempted. A manual CEC client, fallback bridge or other
TX owner must also be absent; exclusive TX requests reject competing owners.
The checks do not prevent another service starting afterward: keep a maintenance
window and verify no automatic restart/other command source can race the test.
The disconnected flag is an operator assertion, not electrically verified.

GPIO13 DATA_RX and GPIO26 BUSY_RX are left exclusively to the independent
`xs8_sniff --event-order`. The Arduino Micro ADC recorder is separately operated
in parallel; verify its inputs, voltage compatibility and common ground without
changing the tested fixture. This tool neither opens serial nor configures the
Arduino. Its ADC sampling and the GPIO event stream are different observations.

## Build and offline checks

Native Pi with libgpiod 2.2.1:

```sh
make .build/xs8_tx_replay CFLAGS='-O2 -Wall -Wextra -Werror'
```

Host-only tests use stub GPIO, clock, RT and replay operations:

```sh
make .build/xs8_tx_replay_test .build/xs8_gpio_tx_only_test
./.build/xs8_tx_replay_test
./.build/xs8_gpio_tx_only_test
make test
```

For Apple Silicon use the existing Raspberry Pi OS-compatible image from
[pi-build.md](pi-build.md). A separate output directory preserves existing
production/RC artifacts:

```sh
mkdir -p .build/pi-tx-replay
docker run --rm --platform linux/arm/v6 --network none \
  --env QEMU_CPU=arm1176 \
  --mount "type=bind,src=$PWD,dst=/src,readonly" \
  --mount "type=bind,src=$PWD/.build/pi-tx-replay,dst=/out" \
  kenwood-xs-pi-build:trixie-armv6 sh -eu -c '
    make -B BUILD_DIR=/out \
      CFLAGS="-O2 -Wall -Wextra -Werror -march=armv6 -mfpu=vfp -mfloat-abi=hard -marm" \
      /out/xs8_tx_replay /out/xs8_sniff /out/xs8_tx_replay_test /out/xs8_gpio_tx_only_test
    /out/xs8_tx_replay_test
    /out/xs8_gpio_tx_only_test
    /out/xs8_tx_replay --dry-run
    file /out/xs8_tx_replay
    readelf -h -A -l -d /out/xs8_tx_replay
    /lib/ld-linux-armhf.so.3 --list /out/xs8_tx_replay
    sha256sum /out/xs8_tx_replay
  '
```

Only stub tests and dry-run execute in the container; no hardware is exposed.
Expect ELF32 ARM v6, VFP hard-float, loader `/lib/ld-linux-armhf.so.3`, and
resolved libgpiod.so.3/libc dependencies. Real CLI transmission is never part of
build/tests. The diagnostic is an explicit Makefile target, not added to `all`.
All consumers of changed xs8_gpio.h must be rebuilt together; do not mix objects
from different header revisions or replace the staged RC with these build files.

## Manual staging and initial capture — not executed by development

With separately approved transfer, copy only the new diagnostic into a fresh Pi
review directory; preserve the original production and RC files. For example:

```sh
ssh serg@woodland-pi.local 'mkdir -p ~/kenwood-build-review/tx-replay'
scp .build/pi-tx-replay/xs8_tx_replay \
  serg@woodland-pi.local:kenwood-build-review/tx-replay/xs8_tx_replay
```

Compare Mac `shasum -a 256` and Pi `sha256sum` before use. Do not infer identity
from filename. Use the already validated standalone sniffer, or separately stage
and verify the newly built one under its own name; do not overwrite production.

1. Power off equipment before disconnecting Kenwood/peripheral XS8 connections.
   Verify physical disconnection and the powered fixture configuration. No
   receiver-connected execution is authorized.
2. Confirm both units are already inactive and no fallback/manual GPIO/CEC
   process can start. These instructions contain no service start/stop command.
3. Create a fresh capture directory; start Arduino ADC recording with its
   existing independently validated procedure. Do not invent ADC setup from this
   document. Start the independent sniffer **before replay**, in terminal A:

   ```sh
   xs8_sniff --event-order --post-gap-ms 5000 15 > rx.csv 2> rx.log
   ```

   This records both RX lines with original event order. A first-gap window can
   shorten capture; verify recorder readiness and complete the short initial
   replay promptly. Do not retry if the capture stops or a stream is missing.
4. In terminal B, first inspect the zero-gap control plan (no GPIO):

   ```sh
   ~/kenwood-build-review/tx-replay/xs8_tx_replay --dry-run
   ```

5. With recorders ready and the fixture attended, run exactly once:

   ```sh
   XS8_RT_PRIORITY=5 ~/kenwood-build-review/tx-replay/xs8_tx_replay \
     --receiver-disconnected > tx.csv 2> tx.log
   ```

   This reproduces the explicitly requested FIFO/5 configuration only if existing
   privileges permit it; inherited transmitter diagnostics report fallback. No
   new privilege is granted. A fallback run is not a successful FIFO/5 comparison.
   Omit XS8_RT_PRIORITY for a deliberately non-RT control run, separately planned.
6. Let independent captures finish; stop the ADC recorder with its own procedure.
   Preserve full raw ADC output, rx.csv/rx.log, tx.csv/tx.log, tool checksums,
   UTC time, RT result and fixture/wiring notes. No automatic service restart,
   retry, extra control frames or receiver reconnection follows.

Later, a separately chosen bounded START-to-STOP delay can be inspected with
`--dry-run --start-stop-ms 150` and applied by adding that flag to the one-shot
command. This value is a selected experiment, not recovered incident timing.
Choose capture duration/post-gap window to cover longer selected delays.

## Output and interpretation

The stdout preamble reports mode, direction, GPIO mapping, no RX/BUSY sensing,
settling interval and configured gaps. A CSV table follows:

```text
index,byte,invoked_ns,tx_start_ns,completion_ns,gpio_tx,error
```

- `invoked_ns`: monotonic sample before calling the shared sender.
- `tx_start_ns`: shared sender's actual software frame-start sample, immediately
  before its waveform writes, after release/settling. Zero means it never reached
  that point. It is not a hardware timestamp of the BUSY voltage edge.
- `completion_ns`: actual monotonic sample immediately after sender return,
  including the sender's RT restoration/timing log overhead. On failure this is
  return time, not proof of a complete frame; -1 denotes a clock failure.
- `gpio_tx`: GPIO completion only, never waveform MATCH or receiver execution.
- `error`: captured errno for a failed send; zero for a successful send.

The four per-frame rows are buffered and printed after replay; existing shared
transmitter timing diagnostics remain on stderr. The first two stdout lines are
metadata, not CSV rows: use the header as the start of the table when importing.
The summary reports attempts, cleanup and no waveform verification. GPIO and
sniffer timestamps use the same Pi monotonic clock; Arduino timing requires
independent alignment using recorded edges, not an assumption of a shared clock.

Handled SIGINT/SIGTERM set a flag; the current shared send may finish before the
replay stops. No later frame is started after cancellation is observed. Both LOW
writes are attempted independently on cleanup, including error paths, followed
by resource release. LOW cleanup failure returns an error and warning. SIGKILL,
power loss, stuck syscalls or electrical faults cannot guarantee cleanup.

Twenty edges per full frame (18 DATA, two BUSY) give a nominal four-frame
expectation of **80 events: 72 DATA and eight BUSY**, if initial LOW creates no
additional RX transition and every expected edge is detected. Preserve deviations
rather than normalizing/sorting captures. A global sequence gap alone proves
neither physical edge loss nor correctly executed receiver commands.
