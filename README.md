# AYN Android Hardware Support

Clean-room Android hardware support shared by AYN handheld device ports.

`rsinputd` is a disabled-by-default Android native service for the Odin2 Mini.
It accepts valid RSInput status frames from the fixed controller UART and
emits a standard uinput gamepad. The service rejects every other product
identity before it opens either device node. Its only serial output is the two
fixed RSInput initialization frames required before status reporting.

The protocol encoding, device identity gate, and status-to-input mapping are
host-tested. The Linux I/O layer is intentionally narrow and has not been
executed on a device; a device product must opt in to the disabled init service
separately.

`odinfand` is a native Binder service for the Odin2 Mini. The Odin2 Mini
product includes it explicitly and starts it with Android's `late_start` class.
Its private unstable `com.ayn.fan.IOdinFan/default` interface exposes only Off,
Quiet, and Sport. The host-tested core owns the exact gpio5 PWM paths, serializes
transactions, keeps PWM period separate from tach, disables before every mode
change, and attempts a confirmed Off state on every gated transaction failure.
The device product owns the SELinux and service-context wiring. The daemon
still rejects any product identity other than the exact Odin2 Mini identity
before it touches the fan nodes.

`libayn_fan_status_core` is an independent, read-only Q9 status reader. It
requires both the exact `odin2_mini` product identity and the stock `Q9` retro
identity before reading the observed `gpio5_pwm2` `state` and `duty` nodes. It
has no write adapter and does not inherit the currently unvalidated fan write
policy. In particular, no third `speed` node or unit interpretation is assumed.

Run all host tests on a development host with:

```sh
./scripts/test-host.sh
```

The test entry point uses warnings-as-errors plus AddressSanitizer and
UndefinedBehaviorSanitizer. The same tests are also exposed to Soong as
`ayn_rsinput_parser_test`, `ayn_rsinputd_policy_test`, and
`ayn_fan_service_test`, `ayn_fan_transaction_test`, `ayn_fan_adapter_test`, and
`ayn_fan_status_test`.

For device bring-up, `m ayn_rsinput_selftest` builds a manually invoked,
test-only utility that creates the same `2020:3001` Android gamepad
identity and emits a bounded button/axis sequence. It does not open the
controller UART or touch MCU, fan, or sysfs controls. Capture `dumpsys input`
and `getevent` while it runs to distinguish Android InputReader and keylayout
behavior from physical controller transport faults.

## License

Apache-2.0. See `LICENSE`.
