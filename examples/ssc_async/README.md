# SSC asynchronous FoE example

The host integration example is `tests/test_core_async.c`. It creates an async-only target, a fixed packet buffer, a fixed operation buffer, the core session, the SSC BUSY/retry gate, and a mock Flash target. It sends one EFW header across several DATA packets, retries a pending packet, writes the payload, reads it back, and waits for finalize before the final ACK.

Run it from the repository root:

```sh
make test-core-async CC=clang
```

Expected final line:

```text
PASS: async core header split, SSC BUSY/retry, Flash write/readback and finalize
```

For an SSC application, configure the target descriptor with `asynchronous = true`, `async_ops`, and `async_user_ctx`. Initialize `ecat_foe_core_async_t` with distinct static packet and operation buffers. Initialize the SSC gate with `ecat_foe_core_async_ssc_ops()` and the session as its context, then call `ecat_foe_ssc_init_async()`.

Forward `pAPPL_FoeWriteData`'s `pData`, `Size`, and `bDataFollowing` to `ecat_foe_ssc_on_data_following()`. Call `ecat_foe_ssc_on_main_loop()` regularly from `pAPPL_MainLoop()` so pending Flash operations advance. Adapt the wrapper's C types to the exact SSC 5.12 headers in the target project. The host example verifies the library contract; it does not claim real SSC, TwinCAT, or board validation.
