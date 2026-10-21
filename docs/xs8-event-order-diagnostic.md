# Passive GPIO event-order diagnostic

Standalone investigation for Raspberry Pi B+, Linux 6.18.50+rpt-rpi-v6 and H11.
It requests only GPIO13 (DATA RX) and GPIO26 (BUSY RX), exclusively as inputs,
with both edges and kernel CLOCK_MONOTONIC timestamps. It never requests TX
GPIO6/19, transmits, decodes XS8, restarts services or changes fail-closed policy.

## Build on Apple Silicon

Use the existing [ARMv6 build environment](pi-build.md). If its image is absent,
run `make pi-build` first. Then build the diagnostic and run synthetic tests:

```sh
mkdir -p .build/pi-sniff
docker run --rm --platform linux/arm/v6 --network none \
  --env QEMU_CPU=arm1176 \
  --mount "type=bind,src=$PWD,dst=/src,readonly" \
  --mount "type=bind,src=$PWD/.build/pi-sniff,dst=/out" \
  kenwood-xs-pi-build:trixie-armv6 sh -eu -c '
    make -B BUILD_DIR=/out \
      CFLAGS="-O2 -Wall -Wextra -Werror -march=armv6 -mfpu=vfp -mfloat-abi=hard -marm" \
      /out/xs8_sniff /out/xs8_sniff_test
    /out/xs8_sniff_test
    file /out/xs8_sniff
    readelf -h -A -l -d /out/xs8_sniff
    /lib/ld-linux-armhf.so.3 --list /out/xs8_sniff
    sha256sum /out/xs8_sniff
  '
```

No devices/network are exposed; the capture program is not executed.
Expect ELF32 ARM, CPU v6, VFP hard-float, interpreter
`/lib/ld-linux-armhf.so.3` and resolved libgpiod.so.3/libc dependencies.
Native Pi fallback with libgpiod 2.x:
`make .build/xs8_sniff CFLAGS="-O2 -Wall -Wextra -Werror"`.
Host-only synthetic tests need no libgpiod:
`make .build/xs8_sniff_test && .build/xs8_sniff_test`.

## Manual staging and attended usage

These are later operator steps, not actions performed by the build or tests:

```sh
ssh serg@woodland-pi.local 'mkdir -p ~/kenwood-build-review'
scp .build/pi-sniff/xs8_sniff serg@woodland-pi.local:kenwood-build-review/xs8_sniff
```

On the Pi, compare `sha256sum ~/kenwood-build-review/xs8_sniff` with the build
output. Preserve the production binary/configuration. Before stopping its GPIO
owner, independently establish no active volume/CD hold, research session or
unresolved termination obligation. Otherwise resolve receiver state separately.

```sh
sudo systemctl stop kenwood-xs8.service
systemctl is-active kenwood-xs8.service
```

Continue only if it reports `inactive` and no other GPIO owner is running.
The atomic exclusive RX request is the final ownership check: EBUSY aborts
without stealing ownership or stopping another process.

```sh
~/kenwood-build-review/xs8_sniff --event-order --post-gap-ms 2000 30 \
  > xs8-order.csv 2> xs8-order.log
```

Use sudo for this command only if the account lacks gpiochip permissions.
Without a gap, capture lasts 30 seconds. On the **first** forward sequence
discontinuity, the deadline becomes 2000 ms after that batch's userspace
read-completion time. An early gap shortens capture; a gap near the initial
deadline gets the full window. Further gaps never extend it. SIGINT/SIGTERM
stop early and release inputs.

Default duration: 10 seconds; diagnostic maximum: 3600 seconds.
`--post-gap-ms`: default 1000, range 1–60000, requires `--event-order`.
Existing `xs8_sniff [duration_seconds]` retains `time_us,gpio,edge` CSV.

After exit, review both files before independently deciding to restart the
service. There is no automatic restart or fault reset. A later attended
`sudo systemctl start kenwood-xs8.service` resumes its existing startup policy;
this diagnostic does not establish receiver integrity.

## Output

```csv
timestamp_ns,gpio,edge,global_seqno,line_seqno,batch,batch_index,batch_size,read_ns,sequence_status
1000000,13,rising,1,1,1,0,2,2000000,IN_ORDER
1029000,26,falling,3,1,1,1,2,2000000,GAP
1010000,13,falling,2,2,2,0,1,2500000,LATE
```

- timestamp_ns: unmodified kernel monotonic timestamp, not relative time.
- global_seqno/line_seqno: unmodified kernel counters. A new request is expected
  to start at global sequence 1; missing initial numbers count.
- batch: one-based read number; batch_index: zero-based delivery index;
  batch_size: number returned by that read. Together these preserve boundaries.
- read_ns: monotonic time immediately after read completion, shared by the
  batch. It is not the physical edge time or exact IRQ latency.
- GAP: sequence exceeds the previous high-water mark plus one.
- LATE: a previously missing number subsequently arrived, including across
  batches. It is removed from the unresolved ledger.
- REGRESSION_OR_DUPLICATE: an older number not in that ledger, or zero.
  Sequence reset/wrap is not reconstructed into a new epoch.

Records remain in original delivery order, including timestamp regressions.
stderr summarizes initially missing numbers, late arrivals, unresolved ranges,
regressions and timestamp regressions. Thus `1,2,4,3,5` gives one initial
missing number, one late arrival and zero unresolved; `1,2,4,5` leaves 3
unresolved. An unresolved number means **not observed before capture ended**,
not proven physical edge loss. Coalescing/overwriting, FIFO loss, another
acquisition issue and arrival after cutoff remain distinct possibilities.

## Bounds and limitations

Diagnostic mode preallocates 262144 records (approximately 12 MiB), with no
printing, allocation or file I/O in the collection loop. On storage exhaustion,
collection still ends on schedule, reports dropped records and warns that
analysis is incomplete. Reporting runs after exclusive GPIO ownership is
released. The userspace batch is 256. The kernel request is 4096, but the
reviewed 6.18.50 source caps it at **1024 shared events**; the tool neither
changes nor queries that effective limit. Scheduling/page faults can affect
draining; no RT scheduling is added.

There is no unbounded final drain. A whole read batch is retained even if
processing crosses the deadline. Events still queued/not yet enqueued when
inputs are released are outside capture. Kernel timestamps never cause records
to be discarded/reordered. collection_complete=yes means no collection/storage
error, not proof every physical edge was captured; stop_reason distinguishes
DURATION, FIRST_GAP_WINDOW, SIGNAL and ERROR.

Exit codes: 0 without forward gaps/storage/I/O errors; 1 for a forward gap
(even if every missing number arrives later) or capture/report error; 2 for
invalid arguments. Keep both files when exit status is 1.

Exclusive ownership means this tool **cannot capture simultaneous service TX**.
It can observe independent front-panel/CD traffic or a separately arranged
fixture, but cannot alone reproduce service TX scheduling or identify the
electrical transmitter. The standalone [H11 TX stimulus](xs8-stimulus.md) is an
explicit exception for receiver-disconnected fixtures: it owns only GPIO6/19
while this tool owns GPIO13/26, with the production service stopped. Do not run
the production transmitter alongside it or interpret these diagnostics as
proof of receiver execution.
