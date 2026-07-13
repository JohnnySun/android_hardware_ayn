# AYN Android Hardware Support

Clean-room Android hardware support shared by AYN handheld device ports.

The current implementation is intentionally narrow: `libayn_rsinput_parser`
parses passive RSInput controller status frames from an arbitrary byte stream.
It handles fragmented and coalesced frames, rejects unknown commands and
unexpected lengths, validates checksums, and recovers after malformed input.

This repository does not yet open a UART, initialize controller firmware,
create an input device, or write to hardware. Those integration layers remain
separate until their behavior and recovery boundaries are proven on stock
firmware.

Run the parser tests on a development host with:

```sh
./scripts/test-host.sh
```

The test entry point uses warnings-as-errors plus AddressSanitizer and
UndefinedBehaviorSanitizer. The same tests are also exposed to Soong as
`ayn_rsinput_parser_test`.

## License

Apache-2.0. See `LICENSE`.
