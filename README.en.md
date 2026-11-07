# ECAT — EtherCAT FoE Firmware Upgrade Library

[中文](README.md) | English

ECAT is a portable C99 library for firmware upgrades on EtherCAT slave devices. It targets standalone bootloaders on 32-bit MCUs and provides a FoE upgrade core, firmware container parsing, and storage driver interfaces for integration with your protocol stack, hardware, and partition layout.

> **Project status: under development.** The core logic, SOES adapter, and bootloader example have been tested on a host computer. They have not yet been integrated and tested with a physical board and EtherCAT master. Validate the complete system on your hardware before considering production use.

## What is included

- An MCU-independent upgrade state machine and `.efw` container parser. The core does not allocate dynamic memory.
- Project and hardware identifier checks, streaming CRC32 and storage readback verification. Write addresses and capacity limits come from the device's local configuration.
- An SOES adapter and [bootloader integration example](examples/bootloader_soes/README.md), host-tested using externally obtained SOES source files and hardware stubs.
- A [firmware packing tool](tools/efw_pack.py), host tests, a [container format specification](docs/spec/efw-format-v0.1.md), and an [architecture document](docs/architecture/SAD-v0.1.md).

SOES and Beckhoff SSC source code are not included; obtain them separately. SSC has an existing synchronous adapter with mock contract tests. A single-packet asynchronous BUSY/retry gate is available as a staged component; it is not yet an asynchronous core or storage-driver integration.

## Verification status

| Area | Status |
| --- | --- |
| Upgrade core and firmware container | Host unit tests with a simulated storage driver |
| SOES adapter and bootloader example | Host tests using real SOES source files and hardware-port stubs |
| SSC | `bDataFollowing` callback wrapper and BUSY/retry gate pass host contract tests; real stack and async core are not verified |
| MCU, Flash, and EtherCAT master | Physical hardware integration not yet tested |

Host tests do not establish timing, storage reliability, or master compatibility on a particular MCU. See the [SOES integration analysis](docs/architecture/soes-integration-options.md) for the source baseline, file hashes, and limitations. Four key files match an identified upstream SOES commit.

## Getting started

The host test suite requires a C99 compiler, Python 3, `make`, and `pdftotext` for PDF citation checks:

```sh
make test CC=clang
```

Optionally run address and undefined-behavior sanitizer checks:

```sh
make test-asan CC=clang
```

For SOES host integration tests, obtain [SOES](https://github.com/OpenEtherCATsociety/SOES), check out the pinned revision, and copy the four test files into a separate directory:

```sh
git clone https://github.com/OpenEtherCATsociety/SOES.git /path/to/SOES
git -C /path/to/SOES checkout --detach 6ef7b9415e936c061ad30626ca00004bdde2ac62
soes_test_dir=$(mktemp -d)
cp /path/to/SOES/soes/esc.c /path/to/SOES/soes/esc.h \
  /path/to/SOES/soes/esc_foe.c /path/to/SOES/soes/esc_foe.h "$soes_test_dir/"
make test-soes SOES_DIR="$soes_test_dir"
make test-bootloader SOES_DIR="$soes_test_dir"
```

SOES versions affect the results; compare the revision and hashes in the [example documentation](examples/bootloader_soes/README.md). These tests use the four upstream files with this repository's host configuration and port stubs. Other headers in a complete SOES directory take precedence over the test stubs, so do not point this test directly at that directory. You may delete the temporary directory after testing. This is not a complete stack integration or physical hardware test.

## Firmware packing CLI

The repository includes the `ecat-efw` command-line entry point, which requires Python 3. Run it from the repository root:

```sh
./ecat-efw pack -i app.bin -p 0x10010001 -n MY_PROJECT -d 1 -t MCU -v 1.0.0.0
./ecat-efw inspect MY_PROJECT_MCU_V1.0.0.0.efw
./ecat-efw --help
```

`-i` is the input file, `-p` the project ID, `-d` the hardware ID, `-t` the target, `-v` the firmware version, and `-o` an optional output path. Long options including `--bin`, `--project-id`, `--hardware-id`, `--target`, `--fw-version`, and `--out` are also available. The IDs and target must match the device configuration. `-n` specifies the project name and must match the slave configuration; it defaults to `PROJECT`.

By default, the tool generates a protocol-conforming filename. If you use `-o` to choose another filename, the filename sent over FoE must still follow the device's naming convention. The tool packages an existing `.bin`; it does not compile firmware and does not overwrite existing output by default. Use `unpack --extract-bin` to extract the payload. The CLI's messages are currently in Chinese.

### Windows executable

GitHub Actions builds and tests a standalone Windows x64 `ecat-efw.exe`; Python is not required on the target Windows computer. A push to the main branch, a pull request, or a manual workflow run produces a build artifact. Pushing a prerelease tag such as `v0.1.0-alpha.1` creates a GitHub prerelease with a ZIP and SHA-256 checksum only after the tests pass. Check the actual workflow and Release status for available versions.

Download, verify, and extract the ZIP, then run `.\ecat-efw.exe --help` in PowerShell. The `pack`, `inspect`, and `unpack` options are the same as above. The executable is currently unsigned. On Windows, `--overwrite` is unavailable because atomic no-follow protection is not available; choose an output path that does not already exist.

## Porting to an MCU

1. **Prepare the device project:** add the library to a standalone bootloader, integrate an EtherCAT slave stack, and configure the target MCU's build and linker.
2. **Configure device and partitions:** set the project ID, hardware ID, storage base address, and capacity. Storage boundaries must come from local device configuration.
3. **Implement the storage driver:** provide initialization, invalidation, erase, write, readback, finalization, and failure cleanup for the target medium. Verify erase/write granularity, address boundaries, and power-loss recovery.
4. **Integrate the protocol stack:** for SOES, follow the example to add the pre- and post-processing hooks and run the stack and hooks serially in one execution context. Other stacks require a separate adapter.
5. **Package and validate:** create an `.efw` container with the packing tool, then validate normal upgrades, error handling, power-loss recovery, and boot behavior with a physical board and master.

See the [bootloader porting example](examples/bootloader_soes/README.md) for interfaces, board configuration, and integration steps, and the [EFW specification](docs/spec/efw-format-v0.1.md) for the container format. Adapt the example configuration and driver to your hardware; they are not validated production firmware.

## Important limitations

- In the tested SOES source, the zero-length final-packet path may ignore a close callback error. A final ACK alone cannot prove the firmware was committed; integrators must provide and validate an independent upgrade-result indication.
- Native SOES filename matching has short-prefix and empty-name pitfalls. Review the adapter and configuration as described in the [SOES integration analysis](docs/architecture/soes-integration-options.md). If read support is not needed, leave `FOE_READ_SUPPORTED` undefined rather than defining it as zero.
- CRC32 detects accidental corruption; it does not authenticate the firmware or protect against malicious modification. Project and hardware IDs are not authentication credentials. Products needing security must implement their own signature verification and trust chain.
- Power-loss recovery depends on the storage driver, persistent validity marker, and bootloader startup logic. Validate Flash erase/write timing, watchdog behavior, and master timeouts on the target device.
- The SSC asynchronous callback now routes `bDataFollowing` through the BUSY/retry gate, but DATA processing still comes from application callbacks. It is not yet connected to the upgrade core or a nonblocking Flash driver and is not a complete asynchronous upgrade path; see the [stage note](docs/architecture/ssc-async-stage1.md).

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
