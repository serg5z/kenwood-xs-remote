# XS8 waveform failure snapshots

Diagnostic-only, default off. This records what the GPIO event API delivered;
it cannot prove that an edge existed on the wire or that Kenwood executed STOP.
TX, decoder tolerances, global-gap policy, STOP/ROOT ordering, retries and fault
latching are unchanged. No automatic retry, recovery or transmission is added.

## Enable and collect on the Pi (operator actions, not deployment)

Use a separately reviewed/checksummed build, retain the production binary and
settings, and establish exclusive GPIO ownership under the existing safety
procedure. Do not start another GPIO owner, sniffer or stimulus alongside the
service. This document does **not** authorize a receiver-connected motor test.

For an independently authorized foreground service session, prefix its existing,
reviewed invocation with `XS8_FAILURE_SNAPSHOTS=1`, for example the shell form:

```sh
XS8_FAILURE_SNAPSHOTS=1 /path/to/reviewed/xs8-service [existing-reviewed-options] \
  > /path/to/new-session.log 2>&1
```

The bracketed arguments and paths are placeholders, not a runnable launch plan.
Keep the existing global-gap and RT settings; this feature requires neither a
policy change nor new GPIO configuration. Any value other than exactly `1`
leaves snapshots off. Allocation failure disables only snapshots, with a warning;
verification still uses its existing decisions.

Startup reports the allocation size and `output=MONITOR_SHUTDOWN_ONLY`.
INVALID verification stores a descriptor in memory. **No snapshot is formatted
or printed on the retry path.** After physical safety is independently established,
end the foreground service with its existing orderly SIGINT/SIGTERM procedure;
existing owed STOP/ROOT cleanup runs before monitor shutdown emits snapshots.
For a separately approved managed session, the equivalent orderly service-stop
procedure must retain stderr/journal output. Do not use SIGKILL to collect data.
No service start/stop or installation is performed by the diagnostic feature.

Preserve the complete log, binary hash, configuration, physical observations and
trial identity. A crash, power loss or forced termination can lose all in-memory
snapshots. Logging at shutdown can delay process exit, but occurs after service
cleanup and is never used to decide whether cleanup succeeded.

## Bounded storage and interpretation

- One preallocated **2048-event ring**, plus **16 failure descriptors**. Storage
  is touched at startup before commands are accepted. There is no per-event or
  per-failure heap allocation, formatting, file write, wait or extra GPIO call.
- Every delivered event is retained in original order, with original kernel
  timestamp, GPIO/polarity, global/per-line counters, read-batch ID/index/size,
  decoder stage/bit count **before** the edge, elapsed interval, decoder error
  **after** the edge, emitted-frame reason and preceding tail state.
- Full retained history before each decision is dumped, including pre-TX context
  rather than clipping a possibly relevant overlapping frame at TX start. This
  comfortably covers ordinary 20-event XS8 frames and a 5 ms pre-start margin;
  event floods/long sessions can exceed any fixed capacity.
- Available events after the decision are included up to TX end +5 ms, without
  waiting for future traffic or delaying retries. `post_margin_reached=0` means
  that a timestamp at least 5 ms past TX end was not present at shutdown. `1`
  denotes an available later timestamp, **not** proof of complete acquisition.
- Later MATCH does not erase earlier failure descriptors. Continuous traffic
  can overwrite raw history; more than 16 failures overwrite older descriptors.
  `overwritten_events`, `overwritten_snapshots`, `raw_first`, `raw_last` and
  `decision_event` expose these limits. Missing history is never synthesized.
- `expected_min_ns/expected_max_ns=0,0` means no numeric range applies to that
  recorded transition. DATA ranges describe the current decoder tests; the
  BUSY-fall guard range describes the stricter local-TX tail predicate.
  `interval_ns` is relative to the decoder's previous phase boundary, not
  necessarily the immediately preceding global event. Negative values preserve
  timestamp/phase inconsistencies rather than sorting them away.
- A frame with `LOW_TIMING` can still contain eight counted bits. `TRUNCATED`
  is an emitted incomplete frame with no other recorded decoder error; it is
  not the TX observation timeout. The raw events help distinguish these cases.

### Verifier branch codes

| Code | Existing rejection condition |
|---|---|
| `DECODE_INVALID` | Selected candidate failed passive frame validity. |
| `LOCAL_TAIL_INVALID` | Passive-valid candidate failed strict DATA-before-BUSY tail verification. |
| `MULTIPLE_CANDIDATES` | More than one candidate starts in the TX window. |
| `END_OUTSIDE_WINDOW` | Candidate ends more than 2 ms after caller TX completion. |
| `PRE_TX_OVERLAP` | Earlier-starting observation overlaps TX start. |
| `ACTIVE_FRAME_OVERLAP` | Existing verifier's active partial-frame attribution condition. |
| `EVENT_GAP_RECORD` | Event-gap record encountered after checkpoint. |
| `OBSERVATION_SERIAL` | Selected observation slot has the wrong serial. |
| `DRAIN_LIMIT` | Existing 32-batch ready-event drain limit reached. |

Where several conditions coexist, the code reports the branch selected by the
existing evaluation order, including the later partial-frame override. UNHEALTHY,
MISSING and MISMATCH retain their distinct meanings; this feature snapshots only
INVALID and never promotes any outcome to MATCH.

## Example (synthetic LOW_TIMING, not a hardware observation)

```text
XS8_FAILURE_BEGIN id=1 byte=0x09 start_ns=7001000000 end_ns=7093100000 checkpoint=0 decision_event=20 rejection=DECODE_INVALID raw_first=1 raw_last=20 initial_busy_low=1 policy=strict post_margin_reached=0
XS8_RAW_HEADER serial,timestamp_ns,gpio,edge,global_seqno,line_seqno,batch,batch_index,batch_size,stage,bits,active,interval_ns,expected_min_ns,expected_max_ns,error,emitted_reason,tail_fell
XS8_RAW,4,7013950000,13,rising,4,3,1,3,20,2,0,1,2000000,3000000,12000000,LOW_TIMING,NONE,0
```

The row records a 2 ms DATA LOW where the decoder expects 3–12 ms. The full
block contains all retained edges, not only this explanatory row. No receiver
execution result is implied.

## Offline replay and tests

The existing real-monitor/fake-libgpiod harness reads the **first failure block**
in a saved log. It restores the recorded global-gap policy, initial BUSY sync,
TX window and observation checkpoint. It feeds events through `decision_event`;
later margin edges did not participate in that original decision. GPIO numbers,
timestamps, edge polarity and sequence counters are never changed or sorted.

```sh
make .build/xs8_monitor_test
.build/xs8_monitor_test --replay-failure saved-service.log
.build/xs8_monitor_test --replay-failure saved-service.log --split 1
.build/xs8_monitor_test --replay-failure saved-service.log --split 7
.build/xs8_monitor_test --replay-failure saved-service.log --split 128
```

Without `--split`, original read-batch boundaries are preserved. With it, only
batch grouping changes. Exit 0 means INVALID and its branch were reproduced;
1 means a different result; 2 means missing/unsupported/overwritten input. To
select a later failure, copy its complete BEGIN-to-END block into a separate
file while preserving the original log. Exact replay currently requires retained
history from monitor start (`raw_first=1`); it refuses to guess prior decoder,
sequence or checkpoint state after ring wrap. Even unreplayable blocks still
provide raw interval/stage diagnostics. It does not reproduce IRQ scheduling,
userspace delays, analog voltage or the TX waveform itself.

Generate a safe synthetic example and benchmark without any hardware linkage:

```sh
.build/xs8_monitor_test --example-failure > .build/example-failure.log
.build/xs8_monitor_test --replay-failure .build/example-failure.log --split 1
.build/xs8_monitor_test --benchmark-snapshots
make test
```

Tests cover LOW_TIMING, seven-bit TRUNCATED, multiple candidates, an incompatible
local tail, strict/warn replay with alternative batches, later MATCH retention,
bounded history overwrite and refusal to reconstruct lost history. A real-service
synthetic-GPIO test checks INVALID STOP → retry MATCH → ROOT MATCH → fault still
latched, with identical commands when snapshots are disabled and enabled. Snapshot
output appears only after cleanup/fault handling. The archived H12 UP fixture
`17 F9 09 07` remains unchanged and is included in normal strict/warn tests.

## Overhead and remaining limits

Enabled storage is **181152 bytes on macOS ARM64** and **164640 bytes on ARMv6**,
plus allocator bookkeeping;
disabled operation allocates no history. The startup message reports the exact
size for the target ABI. Full shutdown output is bounded by 16 × 2048 rows plus
headers; it may be several MiB. Normal successful operation adds no per-frame
snapshot log.

Five host synthetic runs of 200000 edges measured 119–160 ns/event disabled and
126–128 ns/event enabled for the full fake read/decode/log loop. This noisy
microbenchmark does not isolate a reliable additive cost or establish Pi timing.
The bounded record writes do add CPU/memory work to RX processing; zero latency
impact is not claimed. No allocation or blocking diagnostic output is added
between STOP attempts, but Linux scheduling, paging and existing logging still
preclude a hard real-time deadline guarantee. Startup pre-touch is not memory
locking. ARMv6/QEMU checks validate builds and behavior, not physical Pi latency.

Compare a failing raw dump with independent electrical recording before assigning
the cause to TX, the H12 divider, GPIO IRQ acquisition or the decoder. HEALTHY,
continuous counters, GPIO TX_OK, waveform MATCH and actual Kenwood execution
remain different evidence levels. STOP uncertainty and fault latching are never
bypassed by these diagnostics.
