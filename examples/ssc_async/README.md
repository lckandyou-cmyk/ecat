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

For an SSC application, initialize `ecat_target_registry_t` and register the firmware target descriptor with `asynchronous = true`, `driver.fw_async_ops`, and `driver.fw_async_user_ctx` (setting `async_safe_session_abort = true` only when `abort_session` safely preserves any existing valid image before `invalidate`). Initialize `ecat_foe_core_t` (`cfg.registry = &registry`), then initialize `ecat_foe_core_async_t` with distinct static packet and operation buffers, initialize `ecat_transfer_t`, bind the initialized session with `ecat_transfer_init_async_firmware(&transfer, &session)`, and configure a separate transfer staging buffer. Initialize the SSC adapter with `ecat_foe_ssc_init_async()`, then initialize its gate with `ecat_foe_transport_async_ops()` and `&adapter.transport` as context. Finally bind the adapter to the same transfer manager with `ecat_foe_ssc_bind_transfer()`.

Forward `pAPPL_FoeWriteData`'s `pData`, `Size`, and `bDataFollowing` to `ecat_foe_ssc_on_data_following()`. Call `ecat_foe_ssc_on_main_loop()` regularly from `pAPPL_MainLoop()` so pending Flash operations advance. Adapt the wrapper's C types to the exact SSC 5.12 headers in the target project. The host example verifies the library contract; it does not claim real SSC, TwinCAT, or board validation.
