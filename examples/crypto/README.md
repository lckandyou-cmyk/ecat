# Optional Ed25519ph verifier adapter

This directory contains a small adapter from the ECAT streaming verifier callbacks to Monocypher 4.0.3. ECAT does not vendor Monocypher or link it in the default build. Obtain the upstream source separately, pin and review the version in the product build, and compile its `monocypher.c` plus the optional `monocypher-ed25519.c` files with the target's normal toolchain. The optional file provides SHA-512 and Ed25519 APIs; the adapter calls `crypto_ed25519_ph_check()` on the 64-byte SHA-512 digest.

Monocypher's upstream documentation describes the project as C99-portable, dependency-free, and available under CC0-1.0 with 2-clause BSD as an alternative. The upstream download page currently identifies 4.0.3 as its latest release, and its changelog says 4.0.3 fixed a timing leak in EdDSA/Ed25519. Re-check upstream advisories and pin/checksum the source used for each product release. This repository does not make a security certification claim.

## Wiring the adapter

Keep the trusted public key table in protected local firmware/configuration. Never derive a flash address or trust a key from the package. The key ID is the first eight bytes of `SHA-256("ECAT-EFW-KEY-ID-v1\0" || raw_public_key)`; `efw_pack info` displays it for a signed package. Provision that ID alongside the 32-byte public key in a `const` table:

```c
static const ecat_foe_trusted_key_t trusted_keys[] = {
    { { /* 8 Key ID bytes from ecat-efw info */ },
      { /* 32 raw public-key bytes from --public-key-out */ } }
};

static ecat_foe_monocypher_verifier_t verifier_state;
static ecat_foe_signature_verifier_t verifier_callbacks;

ecat_foe_monocypher_verifier_init(&verifier_state, trusted_keys,
                                  sizeof(trusted_keys) / sizeof(trusted_keys[0]),
                                  &verifier_callbacks);
core_cfg.signature_verify_enable = true;
core_cfg.signature_verifier = verifier_callbacks;
```

Assign the callbacks before `ecat_foe_core_init()`. The key table and verifier state must outlive the core and the upgrade session. One verifier context supports one in-flight session; serialize FoE operations as the stack requires. If the callback table is missing, unknown key ID is supplied, algorithm is unsupported, or any callback fails, the core rejects the firmware.

The example intentionally does not ship a trusted production key, connect itself to the example board app, or include Monocypher source. A device enabling `ECAT_FOE_SIGNATURE_VERIFY_ENABLE` must supply callbacks and provision the public key through its own trusted build/configuration process; merely setting the macro makes core initialization fail closed until wiring is complete.

## Host interoperability test

The CI job downloads the [upstream 4.0.3 archive](https://monocypher.org/download/monocypher-4.0.3.tar.gz) and checks SHA-256 `8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66` before compiling it outside the repository. The test signs a package with the CLI/PyNaCl, then feeds it through the core and the real Monocypher verifier over a mock storage driver. It also checks rejection of unsigned packages, unknown key IDs, modified signatures, altered metadata and payload with recomputed CRCs, and corrupted storage readback.

To run the same test locally, obtain and verify Monocypher 4.0.3 separately, install PyNaCl into the Python environment, then run:

```sh
make test-monocypher MONOCYPHER_DIR=/path/to/monocypher-4.0.3/src PYTHON=/path/to/python
```

The target fails if the source files or signing dependency are unavailable. This is a host test; it does not verify a board, key provisioning, secure boot, or power-loss behavior.

## Resource measurement

No target MCU is specified, and no Flash/RAM/time measurements are claimed. For a real port, record the exact MCU, Monocypher source checksum, compiler/version, optimization/LTO/section-GC flags, and link map. Compare verifier-disabled and verifier-enabled `.text`/`.rodata`; include `ecat_foe_monocypher_verifier_t`, SHA-512 state and `ecat_foe_core_t` in static RAM; measure stack high-water; and time the full readback plus final verification over several payload sizes using a cycle counter or GPIO. Host timing is not MCU evidence. The signature adapter uses bounded incremental hashing and the core reads flash in 64-byte chunks, but this alone does not establish an MCU's stack or latency budget.

## Security boundary

Signatures authenticate the exact v2 message defined in [`efw-format-v0.2.md`](../../docs/spec/efw-format-v0.2.md). The core verifies bytes read back from the target before `finalize()` or anti-rollback joint commit. On a single-partition device, erasing the old image may happen before the signature can be checked; signature verification does not guarantee availability or power-fail recovery. Keep image-format/boot validation and secure-boot policy in the platform boot chain. FoE password handling remains independent.
