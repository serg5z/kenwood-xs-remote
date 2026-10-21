# Earlier H11 disconnected-fixture captures — revision unconfirmed

Archived 2026-10-21 from the repository-root captures without changing their bytes.
These are earlier H11 diagnostic records; **Rev.B provenance is not established**.
No exact acquisition date, executable hash, supply measurement or complete wiring
manifest accompanies these files. Do not assign a revision from filenames or
relative chronology. Original root files are retained locally, not deleted.

Each directory preserves RX event-order CSV/log and TX CSV/log. `combined`
contains the 16-frame pattern run; `data-5ms`, `busy-5ms` and `data-20ms` contain
single-line runs. TX completion and LOW_WRITES_OK are software evidence, not
independent electrical verification. Consult each original log for actual
counts/options and interruption. FIRST_GAP_WINDOW means a bounded post-gap
capture, not a guaranteed full-duration capture; collection_complete=yes does
not establish physical edge completeness. Global holes alone do not prove lost
physical edges. The separate per-line counters and timestamps remain unmodified.

## SHA-256 (original bytes)

- `busy-5ms/rx.csv`: `ad249be7e3d2d43ca1b6cc2e497f44721f6c2e287f696026df288757a6564275`
- `busy-5ms/rx.log`: `0b4339c5e3a28aea768f80f8ff57d71d94612d09df39cbf375155370c2b55bcc`
- `busy-5ms/tx.csv`: `fe37281105e09a3fec90ce358429b49e794847185bf75095e82eda7847de2dd8`
- `busy-5ms/tx.log`: `8344cddd9e9d28cb090f8fd2c5e4c7a589ab51c592b170b1d9cadf793e3cc483`
- `combined/rx.csv`: `3deffdc5c59e0d6b0b17921686bef605de2173437fc0a2285d012c6793b350d5`
- `combined/rx.log`: `c1607bbe4fc44f39607d869b83e08cdce299330f51c380253075fe56cd8f90df`
- `combined/tx.csv`: `9a299395424f07c27ce29cd30ced596766785235881fedba786e0a3268ca9997`
- `combined/tx.log`: `9dc2edc2cd94bd100295881282505f6ca9d567c2cc139bbca07f1397d29d52b4`
- `data-20ms/rx.csv`: `14c23b2d431fd40aa61220774be4fef3a60a4c185f07a13d621635c1b8000a14`
- `data-20ms/rx.log`: `3ce04d6160529a14db5871a1c7f2f27f03937815d6cebfb29cce21247c01e94b`
- `data-20ms/tx.csv`: `9d9307f9f7619e1fed4ee565abe85a282b41cacdb249b779a2fecc1d85564017`
- `data-20ms/tx.log`: `0f666d1d56c771660a521b05967cedc4bb2d0d0f91ca4364625f5047ccda6af5`
- `data-5ms/rx.csv`: `40926716224db7cf4f0e2b9858b20aa394d11e4cb4f744b633ecd728932d2a08`
- `data-5ms/rx.log`: `69b135ed41a69fd7dccc589b9c532193421f15f40e6b0079aeca4ea670b8c345`
- `data-5ms/tx.csv`: `8f76cca30ff8aeac94471fc5f2b46437d27ad0644fd2790337d46c51b5d103f7`
- `data-5ms/tx.log`: `b98a41d2f1046aa7b41e904a2617b5861e80cad35db42479e71699a7f98afe7a`
