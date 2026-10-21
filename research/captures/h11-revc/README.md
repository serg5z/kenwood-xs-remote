# Confirmed H11 Rev.C capture evidence

Archived 2026-10-21. The operator explicitly identified these four CSV captures
as H11 Rev.C, after installing a 100 nF capacitor between +5 V and GND. This
provenance is stronger than that of the earlier [revision-unconfirmed records](../h11-revision-unconfirmed/README.md).
Exact acquisition timestamps/tool hashes and a complete wiring manifest were
not supplied. The original CSV and three supplied summary logs are preserved
byte-for-byte. No hold-busy summary log was supplied.

Inspect each summary's stop_reason: FIRST_GAP_WINDOW truncates the requested
capture at the bounded post-gap deadline; collection_complete=yes does not mean
the entire intended stimulus was observed. Compare gap incidence per recorded
event/completed frame rather than raw counts across unequal capture lengths.
The combined-10 record contains missing global number 880: DATA falling 879,
line 792, followed by BUSY falling 881, line 88, about 827 µs later. This does
not establish that a physical transition was missing, or which polarity it had.
No late arrival before capture end does not exclude arrival after that cutoff.
No causal capacitor benefit or degradation is established by these unequal runs.

## SHA-256 (original bytes)

- `h11-revc-10runs-summary.log`: `c98e4134c5e3102c0ce9e4934168eac78096af2dfc601412b1bc9361e5ffbf81`
- `h11-revc-10runs.csv`: `eb4eb2222da24cdcdcc3d597c7bed07b5f747212144a3401dce4b002e5b0641b`
- `h11-revc-combined-10-summary.log`: `f06f27768dcd85dd8b4593f9dc7b73db14f62427be9b1162d34b90b1c6252b0e`
- `h11-revc-combined-10.csv`: `b3af503720245c316807a5712539a4d7498319d62551fd1597455c9281266ec3`
- `h11-revc-combined-summary.log`: `f5b9a2973dbbfdc5297931f86b5f933ae900d5e6482ec8ae0b81a32eeefad034`
- `h11-revc-combined.csv`: `a6ab40df459b6fb8a7e8137d5c1ca51240cd76d68df034d6b34ec827775b2afa`
- `h11-revc-hold-busy.csv`: `2391d2c31b007b8a1868c8c1f919fc8b007719d042e23013cbce04cd4ffe51ea`
