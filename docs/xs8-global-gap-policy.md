# Experimental XS8 global sequence-gap policy

The production monitor reads `XS8_GLOBAL_GAP_POLICY` once when it starts:

| Value | Behavior |
|---|---|
| Unset or `strict` | Default. Every global discontinuity fails monitor health, as before. |
| `warn` | Experimental. A forward global-only gap is logged and counted without automatically failing monitor health or resetting frame decoding. |
| Any other value, including an empty value | Monitor startup fails with `EINVAL`. |

This change does not enable warn mode in any service configuration. It is not
hardware validated. Do not enable it for unattended control on the strength of
the offline results alone. The standalone `xs8_sniff` and CLI self-capture retain
their existing sequence policies; this variable configures `xs8_monitor` only.

## Integrity and waveform checks

BUSY and DATA line sequence counters are checked independently from global
counters, starting at 1 for each requested line. Per-line gaps or regressions,
global regressions/duplicates, timestamp regressions, unexpected GPIO offsets,
event-read failures and detected decoded-buffer overflow remain fail-closed in
both modes. A global gap combined with another integrity fault is not exempt.
Per-line checking also strengthens strict mode for counter faults that the old
global-only check could not detect.

Warn mode only exempts a forward gap with a continuous counter on the delivered
line and no timestamp fault on that event. The other line is checked whenever
its next event arrives; this is not proof that no event is pending or missing.
The raw event order, timestamps, global counters and line counters are retained.
Only the decoder's expected-global-sequence bookkeeping advances over an exempt
gap. No edge is fabricated, sorted, retransmitted or replaced. Bit timing,
BUSY synchronization, protocol-error handling and local-TX tail validation are
unchanged.

Local waveform verification still requires a valid eight-bit frame with the
requested byte and an observed final DATA fall strictly before BUSY falls.
Its final HIGH must be 3–7 ms and its positive DATA-to-BUSY guard at most 2 ms.
Passive validity is deliberately weaker: archived BUSY-first and sampled-together
tails remain supported. A passively valid byte without a verified local tail is
not a local-TX `MATCH`. Neither result proves receiver execution.

## Diagnostics

### Local-TX attribution boundaries (implementation design)

The caller supplies a decoded-history checkpoint before TX, a monotonic start
timestamp taken before the first GPIO write, and a completion timestamp taken
after the transmitter returns. Completion includes scheduling restoration and
TX diagnostic logging; it is not an independently measured final bus edge.

The conservative candidate-start window is inclusive `[start, completion]`.
A completed candidate may end at most 2 ms after completion (the existing
observation allowance). A frame beginning earlier and overlapping start is
invalid attribution. Exactly one complete candidate must satisfy byte and strict
local-tail validation. Multiple candidates, a relevant malformed frame, or an
additional unfinished frame beginning inside the window make attribution
INVALID. With no completed candidate, the existing observation timeout produces
MISSING. An unfinished first candidate may complete within that existing timeout.

Before returning MATCH/MISMATCH, examine completed records, current partial
state and all immediately readable RX batches. There is no fixed extra sleep.
Ready-queue draining is bounded to 32 batches per scan; exhaustion returns
INVALID. Waiting for an unfinished first candidate uses the existing timeout;
history exhaustion fails closed. Traffic beginning strictly after completion is outside attribution and
is retained separately, including malformed/unfinished protocol observations;
genuine capture faults still fail health regardless of timestamp.

This distinguishes subsequent multi-master traffic only when timestamps place
its start outside the window. A response beginning before the caller records
completion is conservatively ambiguous, even if it followed an apparent echo.
Do not reject all traffic after an echo or invent a transmitter identity.
An empty userspace-readable queue is not proof that no kernel IRQ delivery is
still pending; a later integrity failure cannot retroactively revoke a reply.

Per-TX warning counts cover the observation attempt, from just before sending
through verification return, and may conservatively include unrelated queued
traffic. Per-frame warning metadata identifies narrower frame-spanning warnings.
Operational health, cumulative capture warning confidence, and per-TX confidence
must be exposed separately. Receiver inference must not consume gap-spanning
frames as complete-integrity evidence.

Startup logs the selected policy and marks warn mode EXPERIMENTAL. Each exempt
gap is logged after its read batch with raw expected/actual global counters,
GPIO, per-line sequence, event index, monotonic nanosecond timestamps and the
decoder's pre-event frame start, bit count and stage. A zero frame-start value
means no frame was active. These identify the gap relative to decoded-frame
timestamps; they do not attribute an electrical transmitter.

Existing global gap/missing-number/regression counters remain populated.
Shutdown adds policy, warning count and independent per-line gap, missing-number
and regression totals. Warnings do not occupy the first-fatal-fault record or
hide a later fatal integrity error. No logging is added to the GPIO TX path.

### Service STATUS fields

Existing fields retain their meanings and the following fields are appended:

| Field | Meaning |
|---|---|
| `global_gap_policy` | Selected `strict` or experimental `warn` policy. |
| `global_gap_warnings` | Cumulative exempt global-only gaps since monitor startup. |
| `capture_integrity` | `FAILED`, `WARN_GLOBAL_GAP`, or `NO_KNOWN_GAPS`; cumulative, independent of operational `monitor=HEALTHY`. |
| `last_tx_global_gap_warnings` | Exempt gaps delivered during the most recent TX observation attempt, including queued unrelated traffic. |
| `last_tx_capture_integrity` | `NOT_OBSERVED`, `FAILED`, `WARN_GLOBAL_GAP`, `NO_KNOWN_GAPS`, or `NOT_ESTABLISHED`. Waveform evidence remains a separate field. |

`NO_KNOWN_GAPS` means no detected counter/integrity fault in that scope, never
proof of complete physical-edge capture. `MATCH` with `WARN_GLOBAL_GAP` is
possible only for a unique valid local waveform passing byte and tail checks;
it is not full capture-integrity evidence or proof of receiver execution.

Decoded frames carry a frame-spanning warning count. Receiver inference rejects
these frames and marks last-known state `receiver_integrity=UNCERTAIN` whenever
the cumulative warning count changes, including between frames. Active status
queries and CD presence probes terminate with `GLOBAL_GAP_WARNING` if a warning
is delivered during their observation attempt. A later clean explicit status
query can reconcile receiver state; cumulative warning history remains visible.
No automatic query or retransmission is added. The Gateway accepts these fields
as a validated optional group; older service replies remain supported, while
partial groups, invalid values and unrelated unknown fields remain rejected.

## Offline regression outcomes

| Synthetic trace | Strict mode | Warn mode |
|---|---|---|
| Complete matching waveform, continuous counters | MATCH | MATCH |
| Complete matching waveform, global jump 2→5, continuous per-line counters | Monitor FAILED | Warning; MATCH |
| Missing internal DATA edge, original counters retained | Monitor FAILED | Monitor FAILED: global and per-line gap |
| Missing internal DATA edge, counters made dense | INVALID waveform | INVALID waveform |
| Missing final DATA fall, original global counters retained | Monitor FAILED | Warning; passive byte valid, local verification INVALID |
| Missing final DATA fall, counters made dense | Local verification INVALID | Local verification INVALID |
| Extra BUSY rising during DATA activity | INVALID waveform | INVALID waveform |
| Per-line gap with continuous global counter | Monitor FAILED | Monitor FAILED |
| Invalid local tail order/timing | No MATCH | No MATCH, including with a global-only gap |
| Truncated frame plus global-only gap | Monitor FAILED | Warning; no complete observation, MISSING |

Missing internal DATA and extra BUSY variants also remain INVALID when combined
with an exempt global-only gap. Archived BUSY-first, DATA-first and
sampled-together traces retain passive decoding in both policies. The
sampled-together fixture tests both possible event orders and does not assert
electrical simultaneity.

## Residual limitations

- Attribution examines decoded frames and active BUSY-delimited partials.
  Existing passive semantics ignore DATA edges while no frame is active; this
  change does not add a raw-edge grammar or prove the interval was noise-free.
- The kernel cause of a global-only discontinuity is unknown. Warn mode cannot
  distinguish repeated IRQ reservations, physical glitches coalesced before
  numbering, delayed cross-line delivery or other unobserved activity.
- Continuous per-line counters do not establish physical edge completeness.
  Events lost before numbering, or a different physical waveform yielding the
  same delivered valid edges, cannot be distinguished from an intact waveform.
- A lost final event may leave no later same-line counter to expose the loss.
  The missing final DATA fall remains passively decodable under existing
  semantics; the independent local-tail check is what prevents MATCH.
- A future per-line fault cannot retroactively revoke a result already returned.
  A late global number is a regression and fails closed when delivered.
- The GPIO API exposes no independent kernel-FIFO-overflow flag here. Detected
  overflow stays fatal, but not every possible physical/kernel loss is
  identifiable. Existing bounded-history overflow checks remain unchanged.
- Warn mode applies to passive observations as well as local verification.
  Gap-spanning frames remain available as raw observations, but do not establish
  receiver state. Neither warnings nor clean decoding identify the transmitter.

Default strict mode remains the supported production policy. These tests
evaluate specific observable traces, not universal malformed-traffic detection.

## Integration regression coverage

`make test` builds a fake-GPIO service executable linking the real service,
monitor, decoder and verifier. Its Unix-socket requests cover strict/warn valid
TX, duplicate matching frames, a match followed by malformed or unfinished
inside-window traffic, missing local tails, valid/malformed/partial post-window
traffic, warning visibility across successive TX attempts, and an exempt warning
followed by a fatal per-line fault that blocks the next command. Separate monitor
fault-injection tests retain global/per-line regressions, timestamp/read faults,
overflow and malformed-edge coverage. These tests never link the GPIO transmitter.

Synthetic timestamps establish attribution-window behavior, not actual IRQ
latency, electrical frame origin, kernel FIFO completeness or receiver execution.
