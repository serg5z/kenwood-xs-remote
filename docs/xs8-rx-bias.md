# Explicit XS8 RX bias

GPIO13 DATA_RX and GPIO26 BUSY_RX use external voltage dividers. Their internal
pulls must not be inherited from boot configuration, earlier programs or manual
pin configuration. Libgpiod's default bias is AS_IS: requesting INPUT and edge
detection does not implicitly turn off pulls. The kernel specifies that requests
without bias flags leave bias unchanged. See [libgpiod line definitions](https://libgpiod.readthedocs.io/en/master/core_line_defs.html)
and [GPIO v2 request rules](https://docs.kernel.org/userspace-api/gpio/gpio-v2-get-line-ioctl.html).

## Reported H12 evidence

The operator reported a 33 kΩ upper / 47 kΩ lower divider supplied from 4.78 V.
GPIO26-connected HIGH was initially 2.01 V; after a manual no-pull input
configuration it measured 2.81 V, including while xs8_sniff ran. The ideal
unloaded divider calculation is `4.78 × 47 / (33 + 47) = 2.80825 V`.

That is consistent with removal of pull-down loading. Under the ideal divider
assumptions, the 2.01 V reading corresponds to approximately 48.8 kΩ of extra
pull-down loading. This is an inference, not a measured internal resistance or
proof of what configured the pull. Equivalent measurements for GPIO13, the
relationship to event gaps, and physical behavior of these new binaries remain
unverified. This document does not establish a receiver execution result.

## Implementation scope

- [xs8_gpio.c](../xs8_gpio.c) explicitly sets RX bias to
  `GPIOD_LINE_BIAS_DISABLED`. Capture mode applies the same settings to both
  GPIO26 and GPIO13; the pre-existing noncapture path requests/configures only
  GPIO26. Direction, edge detection, monotonic clock and ownership are unchanged.
- [xs8_sniff.c](../xs8_sniff.c) explicitly disables bias for both RX lines in
  both normal CSV and event-order modes.
- Bias setter errors and line-request errors follow existing initialization
  cleanup. There is no fallback to AS_IS or a shell pin-configuration command.
- Output settings, TX waveform/timing, BUSY sensing, RX decoding, local waveform
  verification, strict/warn defaults, STOP obligations/retries and fault latching
  are unchanged by this bias fix.
- TX-only initialization requests only GPIO6/19. It neither requests RX lines
  nor sets bias on RX or TX. It can still coexist with the independent sniffer.

## Support and verification limits

The API is available in libgpiod 2.x, including the target 2.2.1 build environment.
The kernel treats bias as **best effort** on platforms/drivers lacking support:
a successful setter/request or reported line configuration is not independent
proof of the physical pull state. Reported configuration can describe the
request rather than the hardware result. Errors that are returned fail
initialization; the application cannot turn silently ignored hardware support
into an error without a separate reliable mechanism.

The change makes each RX request explicit; it does not configure boot state,
ensure bias persists after the request is released, prevent other privileged
pin-control actions, or compensate for wiring/loading faults. No automatic
pinctrl command, deployment, service restart or physical validation is included.
See the kernel request documentation above for best-effort support and release
semantics.

Later, separately approved disconnected-fixture checks should verify the
requested no-pull state and measure both RX HIGH levels while each new program
owns its lines, then compare captures without assuming that improved voltage
eliminates global gaps. No receiver motor test is required for bias validation.

## Offline regression coverage

[GPIO initializer tests](../tests/xs8_gpio_tx_only_test.c) run the real shared
initializer against fake libgpiod. They assert disabled bias on requested RX,
unchanged AS_IS output settings, zero RX/bias calls for TX-only mode, failure
propagation and resource cleanup for bias-setting and RX-request failures. The
existing waveform and production BUSY checks remain exercised with fake clocks.

[Sniffer setup tests](../tests/xs8_sniff_gpio_test.c) run the actual sniffer main
with fake libgpiod and immediately end capture without events. Both CLI modes
assert input-only GPIO26/13 requests with bias disabled. Direction, bias, edge,
clock, line-configuration and request failures are injected; each must return
failure and free acquired resources. No physical GPIO or TX is used.

```sh
make .build/xs8_gpio_tx_only_test .build/xs8_sniff_gpio_test
./.build/xs8_gpio_tx_only_test
./.build/xs8_sniff_gpio_test
make test
./.build/xs8_monitor_test --replay-csv tests/fixtures/h12-rx-replay.csv --policy strict
./.build/xs8_monitor_test --replay-csv tests/fixtures/h12-rx-replay.csv --policy warn
```

The H12 fixture remains the operator-approved UP sequence `17 F9 09 07`, not the
originally requested DOWN sequence. Replay checks decoder/policy behavior on the
recorded input; it does not physically exercise the new bias setting or establish
STOP execution. See [fixture evidence](../tests/fixtures/README.md).

Strict ARMv6 build/check output for this change is kept separately under ignored
`.build/pi-rx-bias/`; it must not overwrite the staged H11 candidate or production
binaries. Native and ARMv6 stub tests establish requested settings and cleanup,
not actual pin voltages or driver electrical support.
