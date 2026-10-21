# H12 two-channel Arduino Micro ADC edge recorder

Final supplied recorder, copied byte-for-byte from `sample5/sample5.ino` on
2026-10-21. A0 senses BUSY_RX and A1 senses DATA_RX in parallel with the Pi
sense inputs. This is not the legacy Arduino transmitter pin mapping.
No GPIO TX, service or hardware configuration is changed by this archive.

USB serial 115200; send `s` for an up-to-10-second recording. The fixed 160-edge
buffer can stop acquisition sooner. Hysteresis is LOW <=150, HIGH >=250;
151–249 retains the previous state. CSV edges and all/HIGH/LOW extrema/counts
plus ambiguous-band counts are printed after the sampling loop. No dynamic
allocation. Initial state reads are not included in sample statistics.

ADC reads are sequential (BUSY then DATA); timestamps follow conversions, not
electrical edge instants. ADC settling, threshold crossing and loop latency
limit edge/timing fidelity. Min/max are sampled extrema, not proof of absence
of faster transients. Printed HIGH/LOW ranges classify raw thresholds rather
than the retained hysteresis state. The comment estimating HIGH ADC value is
historical, not a calibrated voltage measurement. The source retains its original
extra blank line at EOF; Git whitespace checking reports that archival-only
exception. No matching raw ADC capture or synchronized Pi/Arduino clock record
was supplied with this sketch.

SHA-256: `c613d54916b4ccd3604124de7306dcfbd090088614346b5c645613790eabb28b`
