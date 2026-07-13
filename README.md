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

`odinfand` is a separate, disabled-by-default native fan-service scaffold for
the Odin2 Mini. Its host-tested core contains the observed gpio5 PWM policy,
strict product and sysfs path gates, snapshot validation, and fail-closed write
ordering. A host-tested adapter layer now supplies strict typed settings and
POSIX sysfs boundaries. The binary links the real file adapter, but its
settings source deliberately returns unavailable, so no sysfs I/O is reachable
until a separate, safety-reviewed product change supplies settings and start
wiring.

Run all host tests on a development host with:

```sh
./scripts/test-host.sh
```

The test entry point uses warnings-as-errors plus AddressSanitizer and
UndefinedBehaviorSanitizer. The same tests are also exposed to Soong as
`ayn_rsinput_parser_test`, `ayn_rsinputd_policy_test`, and
`ayn_fan_service_test`, and `ayn_fan_adapter_test`.

## License

Apache-2.0. See `LICENSE`.
