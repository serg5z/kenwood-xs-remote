# H12 RX replay fixture

`h12-rx-replay.csv` is the supplied `xs8_sniff --event-order` capture. Its
SHA-256 is `a4ef7d6c48fafea2d6b76588728d9a651da2c1e99f17cf7504f6fcc7fdb79bc9`.
The test does not rewrite, sort, shift timestamps or renumber this file.

Build and run from the repository root:

```sh
make .build/xs8_monitor_test
./.build/xs8_monitor_test --replay-csv tests/fixtures/h12-rx-replay.csv --policy strict
./.build/xs8_monitor_test --replay-csv tests/fixtures/h12-rx-replay.csv --policy warn
```

This extends the existing fake-libgpiod backend, linked against the unchanged
real monitor, capture-policy and decoder implementations. It incrementally
feeds original read batches, retaining event timestamps, GPIO, polarity, global
and per-line counters. Batch indices/sizes and read timestamps are checked;
userspace scheduling/read delays are not reproduced. No GPIO driver or TX is
linked. Initial BUSY/DATA LOW is an explicit fixture assumption, not an archived
initial-level measurement. Existing synthetic monitor tests also run normally.

## Confirmed recorded sequence and results

The supplied requirement was `17 E9 09 07`. The actual CSV decodes as
**`17 F9 09 07`** (logical waveform bytes). The second frame's bit-4 LOW lasts
10,054,823 ns, above the unchanged 7,350,000 ns short/long split; therefore that
bit is 1, producing F9 rather than E9. No byte-convention conversion is applied
to change those bytes. The operator explicitly approved validating the recorded
**UP** sequence after this discrepancy was reported. The test therefore expects
17 F9 09 07; the original DOWN expectation is preserved here as historical
context, not substituted into the recorded data.

- **Warn:** 80 events in 80 original batches, four valid eight-bit frames,
  continuous per-line counters, no timestamp regression, no incomplete frame,
  no capture fault, exactly three global-only warnings. All four also satisfy
  the existing local-tail predicate. The test asserts the explicitly approved
  UP bytes without modifying the fixture.
- **Strict:** PASS for fail-closed behavior. Processing stops after the first
  fatal batch: event 7, GPIO13 falling, global expected 7 / actual 8,
  line sequence 6. A subsequent manual poll is rejected; TX verification returns
  UNHEALTHY. No later batch is consumed.

Other gap locations are event 19 (expected 20 / actual 21, GPIO13 line 18) and
event 71 (expected 73 / actual 74, GPIO13 line 64); all three reported events are
falling. This does not establish the polarity or existence of a missing physical
transition. Totals are DATA=72 and BUSY=8; global skipped-number total=3.

The default monitor test and `make test` include both H12 policy checks.
Four correctly decoded observed frames, valid tails and HEALTHY capture do not
constitute receiver acknowledgments, actual service-window attribution or STOP
execution confirmation. No transmitter start/end log was supplied to this test.

## Failure snapshot replay

The same offline monitor harness also accepts diagnostic failure logs and can
vary read-batch splits without renumbering edges. See
[waveform failure snapshots](../../docs/xs8-failure-snapshots.md#offline-replay-and-tests).
This does not change the H12 fixture or its expected UP sequence.
