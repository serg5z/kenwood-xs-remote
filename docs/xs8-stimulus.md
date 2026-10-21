# H11 disconnected-fixture TX stimulus

`xs8_stimulus` generates bounded XS8-like transitions on DATA TX GPIO6 and
BUSY TX GPIO19. It requests those two outputs exclusively and never requests
or reads RX GPIO13/26. The independent
[event-order sniffer](xs8-event-order-diagnostic.md) can therefore own RX
simultaneously. No production driver, monitor or safety behavior is changed.

**The Kenwood receiver must be physically disconnected for every run.**
Power equipment off before disconnecting the SYSTEM CONTROL cable, verify
physical separation, then power only the test setup as needed. Keep the
receiver disconnected throughout capture, stimulus, interruption and cleanup.
Do not hot-plug or experiment with this tool on a connected receiver.

## Gates and waveform

- Required acknowledgement: `--receiver-disconnected`. This records operator
  intent; software cannot verify the physical cable.
- Read-only `/usr/bin/systemctl --system --no-pager show` checks the exact
  `kenwood-xs8.service` unit before GPIO setup and again before transmission.
  Only `LoadState=loaded` plus `ActiveState=inactive` with successful command
  exit is accepted. Active, activating, deactivating, failed, missing unit,
  malformed output, missing systemctl/system bus and a two-second query timeout
  all refuse transmission. It never starts, stops or restarts any service.
- Both outputs are requested initially LOW, driven LOW before the first frame,
  and each is independently driven LOW during cleanup before releasing them.
  Exclusive ownership prevents another requester from claiming the same pins.
- Combined mode (default): fixed repeating logical waveform bytes
  **00, FF, 55, AA**. No configurable
  arbitrary-byte mode, receiver-command sequence, retry or recovery operation.
  These bit patterns are not claimed to be NOPs or safe receiver commands.
- Combined default: 16 frames, 100 ms inter-frame gap. Options: `--frames 1..256`,
  `--gap-ms 1..1000`. The gap runs from completion of one frame to the next
  frame's start; there is no final inter-frame wait.
- Planned frame/gap duration must be at most 60 seconds. An elapsed-run check
  and a 250 ms per-frame delay bound abort excessively delayed runs. These
  checks cannot impose a hard wall-clock bound if Linux or GPIO I/O stalls.
- Nominal timings mirror [xs8_gpio.c](../xs8_gpio.c) and the
  [protocol reference](xs8-protocol.md#frame-timing-and-byte-encoding):
  BUSY HIGH; 1150 µs lead; DATA HIGH 9800 µs; eight LSB-first bits with DATA
  LOW 4900 µs for 0 / 9800 µs for 1, each followed by HIGH 4900 µs; DATA LOW
  tail 750 µs; BUSY LOW. There is no RX sensing or BUSY-free arbitration:
  this is an isolated fixture, not the production transmitter.
- CLOCK_MONOTONIC absolute sleeps; no busy wait or RT scheduling.
  SIGINT/SIGTERM/SIGHUP set a flag; normal execution aborts and cleans up.
  GPIO writes and logging are never performed in a signal handler.

The explicit [single-line mode](#single-line-data-and-busy-experiments) below
replaces the frame pattern with alternating transitions; it preserves these
same acknowledgement, service-state, exclusive ownership and cleanup gates.

TX_OK means GPIO calls/waits completed. It proves neither RX delivery nor
receiver execution. Cleanup LOW_WRITES_OK also describes software calls, not
independently measured output levels. On any error, no further frame is sent;
cleanup attempts both LOW writes once, then releases all resources. A failed
LOW write produces a prominent warning.

## ARMv6 build and offline checks

Use the existing [Pi build environment](pi-build.md); if needed, build its image
with `make pi-build` first. No devices or network are mounted for compilation:

```sh
mkdir -p .build/pi-stimulus
docker run --rm --platform linux/arm/v6 --network none \
  --env QEMU_CPU=arm1176 \
  --mount "type=bind,src=$PWD,dst=/src,readonly" \
  --mount "type=bind,src=$PWD/.build/pi-stimulus,dst=/out" \
  kenwood-xs-pi-build:trixie-armv6 sh -eu -c '
    make -B BUILD_DIR=/out \
      CFLAGS="-O2 -Wall -Wextra -Werror -march=armv6 -mfpu=vfp -mfloat-abi=hard -marm" \
      /out/xs8_stimulus /out/xs8_stimulus_test \
      /out/xs8_sniff /out/xs8_sniff_test
    /out/xs8_stimulus_test
    /out/xs8_sniff_test
    for name in xs8_stimulus xs8_sniff; do
      file /out/$name
      readelf -h -A -l -d /out/$name
      /lib/ld-linux-armhf.so.3 --list /out/$name
    done
    cd /out
    sha256sum xs8_stimulus xs8_sniff > SHA256SUMS
  '
```

Expect ELF32 ARMv6, VFP hard-float, loader `/lib/ld-linux-armhf.so.3`,
and resolved libgpiod.so.3/libc dependencies. The build executes only fake
backend tests, not either GPIO tool.
Native Pi build: `make .build/xs8_stimulus CFLAGS="-O2 -Wall -Wextra -Werror"`.
Host tests need no libgpiod:
`make .build/xs8_stimulus_test && .build/xs8_stimulus_test`.

## Manual staging and experiment

These are later attended operator steps, not deployment performed by the
build/tests. Keep production binaries and service configuration unchanged.

On the Mac:

```sh
ssh serg@woodland-pi.local 'mkdir -p ~/kenwood-build-review/h11-stimulus'
scp .build/pi-stimulus/xs8_stimulus .build/pi-stimulus/xs8_sniff \
  .build/pi-stimulus/SHA256SUMS \
  serg@woodland-pi.local:kenwood-build-review/h11-stimulus/
```

Before a run, safely end all existing volume/CD/research operations and resolve
any outstanding termination obligations. Stop the service manually, power off
the equipment and physically disconnect the Kenwood. Keep it disconnected when
powering the Pi/H11 test setup again. Check the service again after any reboot.
On the Pi:

```sh
cd ~/kenwood-build-review/h11-stimulus
sha256sum -c SHA256SUMS
sudo systemctl stop kenwood-xs8.service
systemctl show kenwood-xs8.service --property=LoadState --property=ActiveState
mkdir -p ~/xs8-event-tests/h11-stimulus-001
```

Require loaded/inactive, no other TX GPIO owner, independently verified cable
disconnection, and the existing H11 loopback/sense wiring and supply conditions.
Do not bypass an unknown/failed service state. Use a **new run directory** for
each experiment; preserve raw files without sorting or rewriting them.

**Terminal A — start passive capture first:**

```sh
~/kenwood-build-review/h11-stimulus/xs8_sniff \
  --event-order --post-gap-ms 15000 30 \
  > ~/xs8-event-tests/h11-stimulus-001/rx.csv \
  2> ~/xs8-event-tests/h11-stimulus-001/rx.log
echo "sniffer exit=$?"
```

**Terminal B — while A is capturing, after verifying its process is running,
run one bounded stimulus:**

```sh
~/kenwood-build-review/h11-stimulus/xs8_stimulus \
  --receiver-disconnected --frames 16 --gap-ms 100 \
  > ~/xs8-event-tests/h11-stimulus-001/tx.csv \
  2> ~/xs8-event-tests/h11-stimulus-001/tx.log
echo "stimulus exit=$?"
```

Use sudo for a tool only if the operator lacks gpiochip permissions, retaining
the same arguments and output files. Do not relax device permissions or start
the service to obtain access. Start Terminal B promptly within A's initial
30-second capture. Sixteen ideal frames/gaps take approximately **3.2552 s**;
the two service checks and Linux scheduling add startup/runtime overhead.
A 15-second post-gap window allows capture beyond an early gap without
requiring stimulus to stop at the first diagnostic discontinuity.

Both tools defer CSV/log summaries until capture/TX resources are released.
Let A finish, or interrupt it after B has exited if deliberately ending capture
early. SIGINT on B aborts its waveform and attempts LOW cleanup. Do not rerun
automatically after any failure. Preserve all four files, even on nonzero exit.
The sniffer returns 1 for gaps even when missing numbers subsequently arrive.

Record `uname -r`, library versions, H11 wiring/power arrangement, service state,
tool hashes, options and any observed interruption with the run. No automatic
production restart is included. Later receiver reconnection requires equipment
powered off and a separate attended decision about resuming production.

## Analysis and remaining limitations

Stimulus CSV contains frame index, byte, monotonic start/end nanoseconds,
TX_OK/TX_FAIL, maximum positive wake-up lateness, counts above 500/1000 µs and
sample count. Logs are printed only after GPIO release; no per-frame printing
or allocation occurs during the stimulus. Compare these frame windows with
the original RX CSV timestamps on the **same Pi monotonic clock**.

For ideal non-inverting loopback, a frame contributes 18 DATA edges and 2 BUSY
edges: 320 edges for 16 frames, excluding request/release boundary transients.
This is a synthetic expectation, not independently measured H11 behavior.
Electrical inversion, idle bias or faults must be assessed from the real trace.

Inspect original global/per-line counters and batch boundaries. LATE proves a
previously missing sequence number later arrived. UNRESOLVED means absent by
capture end, not proven physical edge loss. Retain timestamp regressions;
never sort the raw capture. Compare gaps with frame starts/ends and TX jitter.
This experiment can establish whether discontinuities occur outside the
production service; it cannot alone identify kernel coalescing, FIFO overflow,
electrical noise or exact IRQ scheduling as their cause.

Service checks are snapshots. Exclusive GPIO ownership prevents simultaneous
TX ownership, but it cannot prevent a service start attempt or unrelated load
after the checks; avoid timers/manual service starts during testing. No service
unit is masked or modified. Software cannot verify physical disconnection or
guarantee LOW after SIGKILL, crash, power loss, driver failure or GPIO release.
The released-line bias depends on hardware. Keep the Kenwood disconnected
until equipment is powered off and the experiment is finished.

## Single-line DATA and BUSY experiments

Use `--line data` or `--line busy`. Both TX outputs are still exclusively
requested and initialized LOW. By default only the selected output is toggled;
the other receives LOW writes only, during initialization and cleanup. The
explicit [BUSY-held-HIGH experiment](#data-switching-with-busy-held-high)
below is an exception for diagnosing coupling.

- `--transitions N`: default 320, even count **2–10000**. The first transition
  is HIGH and the final transition is LOW. Even counts avoid an extra planned
  cleanup transition. An interrupted/failed run can still generate a cleanup
  LOW edge; check TX logs before comparing against the requested count.
- `--interval-us N`: default **5000 µs**, allowed 1000–20000 µs.
  Transitions follow absolute monotonic deadlines at the requested nominal
  spacing. The final LOW is held for one interval before cleanup, so planned
  duration is transitions × interval: **1.6 seconds** at 320 × 5000 µs.
- The product must be at most 60 seconds. Excessive wake-up lateness above
  250 ms aborts the train; cancellation/errors perform normal LOW cleanup.
  Linux delays can change actual spacing and cause compressed catch-up
  intervals. Review write timestamps and wake-up lateness.
- Do not mix `--frames`/`--gap-ms` with single-line mode, or use transition
  options without `--line`. The combined frame path and CSV remain unchanged.

### Build and staging

Run the ARMv6 Docker command in [ARMv6 build and offline checks](#armv6-build-and-offline-checks)
above. It rebuilds the updated stimulus, existing sniffer and both synthetic
tests with `-Werror`. It executes no GPIO tool.
Additional report tests on the Mac:

```sh
python3 -m unittest tests/test_xs8_event_report.py
```

Stage both rebuilt tools, checksums and the optional read-only Python report:

```sh
ssh serg@woodland-pi.local 'mkdir -p ~/kenwood-build-review/h11-stimulus'
scp .build/pi-stimulus/xs8_stimulus .build/pi-stimulus/xs8_sniff \
  .build/pi-stimulus/SHA256SUMS xs8_event_report.py \
  serg@woodland-pi.local:kenwood-build-review/h11-stimulus/
```

On Pi, after physically disconnecting Kenwood with equipment powered off and
following the existing service-stop prerequisites:

```sh
cd ~/kenwood-build-review/h11-stimulus
sha256sum -c SHA256SUMS
systemctl show kenwood-xs8.service --property=LoadState --property=ActiveState
mkdir -p ~/xs8-event-tests/data-001 ~/xs8-event-tests/busy-001
```

Require loaded/inactive and leave the receiver disconnected for both runs.
Choose fresh directory names rather than overwriting previous evidence.

### DATA run

Start Terminal A first:

```sh
~/kenwood-build-review/h11-stimulus/xs8_sniff \
  --event-order --post-gap-ms 15000 30 \
  > ~/xs8-event-tests/data-001/rx.csv \
  2> ~/xs8-event-tests/data-001/rx.log
```

While A is capturing, run once in Terminal B:

```sh
~/kenwood-build-review/h11-stimulus/xs8_stimulus \
  --receiver-disconnected --line data --transitions 320 --interval-us 5000 \
  > ~/xs8-event-tests/data-001/tx.csv \
  2> ~/xs8-event-tests/data-001/tx.log
```

Let both tools exit. Inspect TX run_complete, cleanup and result, plus the
RX collection/storage status. Preserve raw files even if either tool fails.

### BUSY run

Start a **fresh capture** in Terminal A:

```sh
~/kenwood-build-review/h11-stimulus/xs8_sniff \
  --event-order --post-gap-ms 15000 30 \
  > ~/xs8-event-tests/busy-001/rx.csv \
  2> ~/xs8-event-tests/busy-001/rx.log
```

While that capture is running, use Terminal B:

```sh
~/kenwood-build-review/h11-stimulus/xs8_stimulus \
  --receiver-disconnected --line busy --transitions 320 --interval-us 5000 \
  > ~/xs8-event-tests/busy-001/tx.csv \
  2> ~/xs8-event-tests/busy-001/tx.log
```

Start each stimulus promptly within its capture's initial 30 seconds; do not
reuse one RX request across runs. Do not start production services or another
transmitter between these commands. No automatic service restart is included.

### Report each run without altering captures

The nominal counts, assuming completed TX and ideal loopback, are:

| Run | DATA_RX GPIO13 | BUSY_RX GPIO26 |
|---|---:|---:|
| DATA | 320 (160 rising, 160 falling) | 0 |
| BUSY | 0 | 320 (160 rising, 160 falling) |

All rows in the capture count, including request/release boundary activity.
Counts alone do not prove every intended edge was captured; extra edges can
mask missing ones. Compare TX/RX timestamps and edge types as well.

```sh
python3 ~/kenwood-build-review/h11-stimulus/xs8_event_report.py \
  --line data --expected 320 ~/xs8-event-tests/data-001/rx.csv \
  > ~/xs8-event-tests/data-001/report.json
python3 ~/kenwood-build-review/h11-stimulus/xs8_event_report.py \
  --line busy --expected 320 ~/xs8-event-tests/busy-001/rx.csv \
  > ~/xs8-event-tests/busy-001/report.json
```

The script reads only the RX CSV and never sorts its rows. It reports expected
versus observed counts, edge types, global and per-line continuity independently,
initially missing ranges, late-arriving numbers, still-unresolved ranges,
regressions, timestamp regressions and unexpected inactive-line activity.
No events on a line gives continuity=null (no evidence), not a tested pass.
Check the TX/RX logs separately: the script cannot establish TX completion or
capture integrity from RX CSV alone. Report exit success only means the file
was parsed; counter gaps remain explicit fields in JSON.

Single-line TX CSV is:

```text
transition,gpio,level,write_ns,deadline_ns,wake_ns,gpio_result,wait_result
```

write_ns is sampled immediately before the GPIO write; it is not a measured
electrical transition. deadline_ns is the following absolute sleep deadline.
GPIO TX_OK and WAIT_OK describe software operations, not physical RX evidence.
After releasing GPIO, stderr reports attempted/written counts, whole-train
completion, timing statistics and LOW cleanup outcome.

Retain rx.csv, rx.log, tx.csv, tx.log and report.json for each run. Copy the
complete run directories to the analysis machine without modifying the raw
files. A global gap with contiguous per-line counters is different evidence
from a per-line discontinuity, and neither alone proves physical edge loss.
If the inactive RX line shows events, the experiment is no longer electrically
single-line even though the tool never drove that TX HIGH. Isolate that
observation before attributing gaps solely to cross-line scheduling.

## DATA switching with BUSY held HIGH

This **disconnected-fixture-only** mode investigates possible DATA-to-BUSY
coupling. It is not an XS8 receiver command or a production control sequence.
All existing physical-disconnection, loaded/inactive service, exclusive TX
ownership, bounded-runtime and LOW-cleanup guards apply.

Add `--hold-busy-high --line data`. The flag is rejected without `--line data`
or with `--line busy`/combined mode. Reuse `--transitions` and `--interval-us`;
ordinary single-line and combined modes remain unchanged.

Order of operations:

1. Request GPIO6 and GPIO19 initially LOW; complete the normal service checks.
2. Assert BUSY_TX GPIO19 HIGH.
3. Wait one configured interval to separate initialization from DATA activity.
4. Toggle DATA_TX GPIO6 for the requested even count. BUSY receives no writes
   during this train and remains requested HIGH.
5. Hold the final DATA LOW for one interval, as in ordinary single-line mode.
6. Cleanup attempts DATA LOW and BUSY LOW independently, then releases GPIO.

The initial settling interval is included in the 60-second planned-run limit:
`(transitions + 1) × interval_us`. At 320 transitions and 5000 µs, nominal
BUSY-HIGH duration is approximately **1.605 seconds**, plus GPIO/scheduling
overhead. No retries occur. Cancellation during the settling interval or DATA
train still attempts both LOW writes. Failed cleanup is explicitly warned;
software cannot guarantee physical levels after errors, crash or power loss.

### Build, stage and capture

Re-run the ARMv6 Docker build in [ARMv6 build and offline checks](#armv6-build-and-offline-checks).
It builds with `-Werror` and executes fake-backend tests, including BUSY
assertion/hold/release ordering, expected synthetic RX counts, cancellation,
write failures and invalid modes. Stage the rebuilt tools using the existing
[build and staging](#build-and-staging) commands and verify SHA256SUMS.
Neither command deploys or starts the production service.

With the receiver physically disconnected and kenwood-xs8.service confirmed
loaded/inactive, create a fresh directory:

```sh
mkdir -p ~/xs8-event-tests/data-busy-high-001
```

Terminal A first — unchanged passive capture of **both** GPIO13 and GPIO26:

```sh
~/kenwood-build-review/h11-stimulus/xs8_sniff \
  --event-order --post-gap-ms 15000 30 \
  > ~/xs8-event-tests/data-busy-high-001/rx.csv \
  2> ~/xs8-event-tests/data-busy-high-001/rx.log
```

While A is capturing, Terminal B:

```sh
~/kenwood-build-review/h11-stimulus/xs8_stimulus \
  --receiver-disconnected --hold-busy-high --line data \
  --transitions 320 --interval-us 5000 \
  > ~/xs8-event-tests/data-busy-high-001/tx.csv \
  2> ~/xs8-event-tests/data-busy-high-001/tx.log
```

The stimulus owns only TX GPIO6/19; the sniffer owns only RX GPIO13/26. They
can run concurrently without sharing a line request. The production service
must remain stopped. Preserve all four files, including unsuccessful runs.

### Expected observations and diagnostics

For a completed run and ideal non-inverting loopback:

| RX line | Expected events |
|---|---|
| DATA GPIO13 | 320 alternating edges: 160 rising, 160 falling |
| BUSY GPIO26 | Exactly 2: rising before DATA begins, falling during cleanup |
| BUSY during DATA switching | No edges |

These are expectations, not hardware validation. Count and inspect the BUSY
rows in original delivery order and compare their kernel timestamps with DATA
activity. Extra events or an apparent BUSY drop during DATA are observations
to investigate, not by themselves proof of electrical coupling. Missing or
coalesced events can also affect the recorded picture.

Startup and completion logs identify `mode=DATA_WITH_BUSY_HIGH`, both TX
GPIOs, requested/attempted/written DATA counts, train completion and cleanup.
The BUSY_HOLD log records assertion attempt/result, pre/post-write monotonic
timestamps, settling-wait completion, and cleanup LOW timestamp/result.
These timestamps and TX_OK describe software calls; no RX pin is read and
physical level/execution is never inferred. All results are printed after
GPIO release. The DATA transition CSV format is unchanged.

The existing read-only event report assumes the nonselected RX line should
have zero events. In this experiment its `unexpected_inactive_line_events=2`
would be expected: **BUSY is deliberately held HIGH**, not inactive. Use its
raw counts/counter-continuity fields, but compare BUSY against 2 manually and
inspect when those edges occurred. It cannot automatically distinguish the
two expected boundary edges from extra activity during DATA switching.

## BUSY-HIGH-only control experiment

This disconnected-fixture control holds BUSY HIGH with **no DATA switching**.
It helps compare GPIO26 activity against the DATA-switching experiment above.
It requests only the existing TX GPIO6/19, both initially LOW. It preserves the
same physical-disconnection acknowledgement, loaded/inactive service checks,
exclusive GPIO ownership, signal handling and independent LOW cleanup.

```sh
xs8_stimulus --receiver-disconnected --hold-busy-high-only --duration-ms 3000
```

`--duration-ms` is required and must be **1–60000 ms**. It is rejected without
`--hold-busy-high-only`. Do not combine this mode with `--line`, `--frames`,
`--gap-ms`, `--transitions`, `--interval-us` or `--hold-busy-high`.
Existing modes and their waveform/CSV formats remain unchanged.

The tool keeps DATA_TX LOW, asserts BUSY_TX HIGH once and sleeps until an
absolute CLOCK_MONOTONIC deadline measured from completion of the HIGH write.
It then attempts DATA LOW and BUSY LOW independently and releases both outputs.
There are no intermediate GPIO reads, refresh writes, DATA transitions, retries
or receiver recovery commands. SIGINT/SIGTERM/SIGHUP interrupt the wait and take
the same cleanup path. GPIO/clock errors also take cleanup; a timestamp failure
cannot skip the BUSY LOW attempt.

The requested hold is capped at 60 seconds. Actual time HIGH includes GPIO
overhead and possible Linux scheduling lateness; this is not a hardware-enforced
cutoff. Excessive wake-up lateness is reported as an error after cleanup.
Failed LOW writes produce a warning. Crash, SIGKILL, power loss or hardware
failure can prevent software from establishing LOW: keep Kenwood disconnected.

### Build and manual capture

Rebuild with the existing [ARMv6 Docker command](#armv6-build-and-offline-checks)
and stage both diagnostic binaries/checksums using [build and staging](#build-and-staging).
The strict build uses `-O2 -Wall -Wextra -Werror` and ARMv6 hard-float flags.
Regression tests cover the 1/3000/60000 ms cases, invalid/mixed modes, expected
synthetic RX edges, cancellation, service/open failures, GPIO failures and
clock errors including cleanup timestamps. No hardware tool is run by tests.

After independently verifying physical disconnection and a loaded/inactive
kenwood-xs8.service, create a new run directory:

```sh
mkdir -p ~/xs8-event-tests/busy-high-only-001
```

Terminal A first — capture both RX lines:

```sh
~/kenwood-build-review/h11-stimulus/xs8_sniff \
  --event-order --post-gap-ms 15000 30 \
  > ~/xs8-event-tests/busy-high-only-001/rx.csv \
  2> ~/xs8-event-tests/busy-high-only-001/rx.log
```

While A is capturing, Terminal B:

```sh
~/kenwood-build-review/h11-stimulus/xs8_stimulus \
  --receiver-disconnected --hold-busy-high-only --duration-ms 3000 \
  > ~/xs8-event-tests/busy-high-only-001/tx.csv \
  2> ~/xs8-event-tests/busy-high-only-001/tx.log
```

The unchanged sniffer exclusively owns RX GPIO13/26 while this tool exclusively
owns TX GPIO6/19, so their requests do not conflict. Keep the production service
stopped. The capture settings above are for this 3-second test; longer holds
require a capture window that also covers cleanup and possible gap-triggered
early termination. No service is started/stopped automatically.

### Expected observations and evidence

For a completed run with ideal loopback and capture covering both boundaries:

| Measurement | Expected |
|---|---|
| DATA_RX GPIO13 | 0 events |
| BUSY_RX GPIO26 | Exactly 2 events: rising, then falling |
| BUSY events between these boundaries | 0 |
| Global sequence gaps | 0 |
| BUSY per-line sequence | 1, 2 |

These are test expectations, not a claimed physical result. Inspect all RX
records in original delivery order and preserve both CSVs and logs. Unexpected
GPIO26 events during a stable HIGH hold are evidence to investigate; they do
not alone establish electrical noise, IRQ duplication or physical transitions.

TX stdout records BUSY boundaries:

```text
gpio,level,write_ns,done_ns,gpio_result
```

write_ns/done_ns are monotonic timestamps sampled before/after each GPIO call,
not independently measured electrical transitions. The cleanup LOW is recorded
even if HIGH failed; only attempted operations appear. TX_OK means that GPIO
call returned successfully. stderr identifies mode=BUSY_HIGH_ONLY, GPIOs,
duration, wait deadline/wake time, hold completion, overall result and cleanup.
All results are printed after GPIO resources are released.

The existing event-report helper's single-line transition expectations do not
describe this static control. Compare against DATA=0/BUSY=2 manually and use the
sniffer's raw sequence numbers and summary; do not classify the two intended
BUSY boundary events as unexpected inactive-line traffic.
