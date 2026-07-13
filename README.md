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

Run all host tests on a development host with:

```sh
./scripts/test-host.sh
```

The test entry point uses warnings-as-errors plus AddressSanitizer and
UndefinedBehaviorSanitizer. The same tests are also exposed to Soong as
`ayn_rsinput_parser_test` and `ayn_rsinputd_policy_test`.

## License

Apache-2.0. See `LICENSE`.
