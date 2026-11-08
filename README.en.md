# ECAT: EtherCAT FoE Firmware Upgrade Library

[中文](README.md) | English

ECAT is a C99 EtherCAT FoE firmware-upgrade library for standalone bootloaders on 32-bit MCUs. It provides an upgrade state machine, `.efw` container parsing and validation, and target-storage driver interfaces. Integrators connect these components to their EtherCAT slave stack, hardware platform, and local partition configuration.

> **Project status: under development; product-level validation is incomplete.** The core logic, SOES integration path, and bootloader example have passed the host tests described below. They have not been integrated and tested with a physical MCU board and EtherCAT master. Validate the complete system on the target hardware, stack, and boot flow before product use.

## Scope

- **Upgrade core:** an MCU- and stack-independent C99 state machine with no dynamic allocation in the core.
- **Firmware validation:** project, hardware, target, version, and data-integrity checks, with support for storage readback verification. Write addresses and capacity limits are taken only from device-local configuration.
- **Stack integration:** an SOES adapter and [bootloader integration example](examples/bootloader_soes/README.md). Integrators must obtain protocol-stack source code separately.
- **Tools and documentation:** a [firmware packing tool](tools/efw_pack.py), host tests, the [container format specification](docs/spec/efw-format-v0.1.md), and the [architecture document](docs/architecture/SAD-v0.1.md).

Third-party SOES and Beckhoff SSC source code is not distributed in this repository. The synchronous SSC adapter has Mock contract tests. The asynchronous callback gate is not yet integrated with the upgrade state machine or a nonblocking storage driver. Neither path has been verified against the real SSC.

## Verification status

| Area | Status |
| --- | --- |
| Upgrade core and `.efw` container | Host unit tests with a simulated storage driver |
| SOES adapter and bootloader example | Host tests using pinned SOES source files and hardware-port stubs |
| Beckhoff SSC | Synchronous adapter Mock contract tests and asynchronous gate tests pass; real SSC integration and the asynchronous core path are unverified |
| MCU, Flash, and EtherCAT master | Physical hardware integration not yet tested |

Host tests do not establish timing, storage reliability, power-loss recovery, or master compatibility on a target MCU. See the [SOES integration analysis](docs/architecture/soes-integration-options.md) for the source baseline, file hashes, and limitations. The test uses four upstream source files with this repository's host configuration and hardware-port stubs; it is not a full stack or hardware integration test.

## Host development and tests

The host test suite requires a C99 compiler, Python 3, `make`, and `pdftotext` for PDF citation checks:

```sh
make test CC=clang
```

Optionally run AddressSanitizer and UndefinedBehaviorSanitizer checks:

```sh
make test-asan CC=clang
```

Before running SOES host integration tests, obtain [SOES](https://github.com/OpenEtherCATsociety/SOES), check out the revision specified by this project, and copy the four test files into a separate directory:

```sh
git clone https://github.com/OpenEtherCATsociety/SOES.git /path/to/SOES
git -C /path/to/SOES checkout --detach 6ef7b9415e936c061ad30626ca00004bdde2ac62
soes_test_dir=$(mktemp -d)
cp /path/to/SOES/soes/esc.c /path/to/SOES/soes/esc.h \
  /path/to/SOES/soes/esc_foe.c /path/to/SOES/soes/esc_foe.h "$soes_test_dir/"
make test-soes SOES_DIR="$soes_test_dir"
make test-bootloader SOES_DIR="$soes_test_dir"
```

SOES versions affect test results. Check the revision and hashes in the [example documentation](examples/bootloader_soes/README.md). These tests use only the four upstream files with this repository's host configuration and port stubs. Other headers in a complete SOES directory may take precedence over the test stubs, so do not use the full source directory as `SOES_DIR`. The temporary directory may be deleted after testing. This is not a full stack integration or physical hardware test.

## Firmware packing CLI

The repository provides the `ecat-efw` command-line tool to package and inspect `.efw` files. The source entry point requires Python 3; run it from the repository root:

```sh
./ecat-efw pack -i app.bin -p 0x10010001 -n MY_PROJECT -d 1 -t MCU -v 1.0.0.0
./ecat-efw inspect MY_PROJECT_MCU_V1.0.0.0.efw
./ecat-efw --help
```

`-i`, `-p`, `-d`, `-t`, and `-v` specify the input file, project ID, hardware ID, target, and firmware version, respectively. Use `-o` to set an output path. Long options including `--bin`, `--project-id`, `--hardware-id`, `--target`, `--fw-version`, and `--out` are also available. The identifiers and target must match the device configuration. `-n` specifies the project name and must match the slave configuration; it defaults to `PROJECT`.

The tool generates a protocol-conforming filename by default. If `-o` specifies another filename, the name sent over FoE must still follow the device's naming convention. The tool packages an existing `.bin`; it does not compile firmware and refuses to overwrite existing output by default. Use `unpack --extract-bin` to extract a payload.

### Windows executable

GitHub Actions builds and tests a standalone Windows x64 `ecat-efw.exe`; Python is not required on the target Windows computer. Pushes to the main branch, pull requests, and manual workflow runs produce build artifacts. Pushing a prerelease Git tag such as `v0.1.0-alpha.1` creates a prerelease only after the tests pass, with a ZIP archive and SHA-256 checksum. Refer to the actual Actions and Release pages for availability and status.

Download and verify the ZIP archive, then extract it and run `.\ecat-efw.exe --help` in PowerShell. The `pack`, `inspect`, and `unpack` subcommands are the same as above. The executable is currently unsigned. The Windows build does not support `--overwrite`, because it cannot reliably provide the required no-follow overwrite protection; specify an output path that does not already exist.

## Porting to an MCU

1. **Prepare the device project:** integrate the library into a standalone bootloader, connect an EtherCAT slave stack, and configure the target MCU build and linker.
2. **Configure the device and partitions:** set project and hardware identifiers, upgrade targets, storage base addresses, and capacity limits. The device's local configuration is the sole source of storage boundaries; firmware packages must not select write addresses.
3. **Implement the storage driver:** provide initialization, image invalidation, erase, write, readback verification, finalization, and failure cleanup for the target medium. Verify erase/write granularity, address boundaries, and power-loss recovery.
4. **Integrate the protocol stack:** for SOES, follow the example to add the pre- and post-processing hooks. Ensure protocol handling, asynchronous gates, and driver operations follow the required execution-context and serialization contract. Other stacks require a separate adapter that must be validated.
5. **Package and validate:** use the CLI to package a firmware image as `.efw`, then validate successful upgrades, error handling, power-loss recovery, and boot behavior on the target board and EtherCAT master.

See the [bootloader porting example](examples/bootloader_soes/README.md) for interfaces, board configuration, and integration steps. The [EFW specification](docs/spec/efw-format-v0.1.md) defines the container fields and validation rules. Adapt and validate the example configuration and driver for the target hardware; they are not production firmware.

## Important limitations

- In the tested SOES source, the zero-length final-packet path may ignore a close-callback error. A final ACK alone does not prove that the image was committed; integrators should provide and validate an independent upgrade-result indication.
- Native SOES filename matching has short-prefix and empty-name edge cases. Review the adapter and configuration using the [SOES integration analysis](docs/architecture/soes-integration-options.md). If read support is not required, leave `FOE_READ_SUPPORTED` undefined; do not define it as zero.
- The SSC adapter checks the FoE access password. Its expected password defaults to `0`; configure a nonzero value before accepting WRQ requests when required. A FoE password provides protocol-level access control, not cryptographic authentication, and does not replace firmware signatures or a trust chain.
- CRC32 detects accidental corruption; it does not authenticate firmware or prevent malicious modification. Project and hardware IDs are not authentication credentials. Products with security requirements must implement signature verification and a trust chain.
- Power-loss recovery depends on the storage driver, persistent image-validity marker, and bootloader startup logic. Validate Flash erase/write timing, watchdog behavior, and master timeouts on the target device.
- The SSC asynchronous callback provides `bDataFollowing` wrapping and BUSY/retry gating, but DATA handling is not connected to the upgrade core or a nonblocking Flash driver. It is not a complete asynchronous upgrade path; see the [stage note](docs/architecture/ssc-async-stage1.md).

## Repository layout

| Path | Contents |
| --- | --- |
| `include/`, `src/` | Core interfaces, upgrade logic, and protocol stack adapters |
| `examples/bootloader_soes/` | SOES bootloader integration reference and host integration tests |
| `tests/` | Unit tests, mock driver, and protocol stack tests |
| `tools/` | Firmware packing and verification tools |
| `docs/` | Architecture, container specification, and integration analysis |

The linked detailed documentation and issue templates are currently in Chinese.

## Reporting issues

Please use the [bug report template](.github/ISSUE_TEMPLATE/bug_report.md) or [feature request template](.github/ISSUE_TEMPLATE/feature_request.md). Include the repository revision, MCU and board model, protocol stack source and version, master and compiler versions, reproduction steps, and sanitized logs where possible. Do not publish passwords, keys, credentials, or proprietary firmware.

## License

This project is distributed under the [MIT License](LICENSE). Use of third-party protocol stacks is governed by their respective licenses.
