# SSC FoE asynchronous adapter: stage 1

This stage adds a single-flight packet gate for an SSC FoE DATA callback. It
copies one packet into caller-owned fixed storage, starts an application-owned
operation once, returns the configured BUSY code while that operation is
pending, and only returns success/error when SSC retries the same packet.

## Current boundary

`ecat_foe_ssc_async` is a protocol adapter primitive, not an asynchronous
firmware-upgrade implementation. It does not call `ecat_foe_core`, does not
provide asynchronous Flash operations, and must not yet be used as a complete
production upgrade path. The core and its target driver remain synchronous.
The next implementation step is an incremental core/driver API that can be
connected to this gate without blocking the SSC main loop.

## Callback contract

- `start` is called exactly once for a newly received DATA block. It must copy
  any state it needs other than the supplied packet, which remains valid in the
  adapter buffer until the retry is acknowledged or the operation is aborted.
- `start` and `poll` return `PENDING`, `COMPLETE`, or `FAILED`. A failed start
  or poll is followed by one `abort` call. `abort` must therefore be safe after
  partial start failure.
- While pending, a callback retry must have identical bytes, length, and
  `data_following`. Matching retries return BUSY without re-running `start`;
  a different packet returns the configured packet error and does not replace
  the outstanding block.
- After `poll` reaches a terminal state, the next identical SSC callback retry
  receives success or the configured operation error. The adapter does not
  send protocol responses itself; SSC sends the response based on the callback
  return value.
- The application must call `ecat_foe_ssc_async_poll()` regularly from its main
  loop. Calls into the callback, poll, and abort functions must be serialized.
- `data_following` is passed through unchanged. In the reported SSC 5.12 source,
  it is derived from whether the current packet fills the available mailbox
  payload; `false` indicates a short/final block, and zero-length termination
  must be tested separately. The source package is not included in this repo,
  so this observation is not independently reproducible from this checkout.
- A nonzero timeout uses unsigned wrap-safe tick arithmetic and must be less
  than half the 32-bit tick range. On timeout the backend is aborted and the
  outstanding packet remains held until SSC retries it and receives the error,
  or the application explicitly aborts the transaction.

## Verification

`tests/test_ssc_async_gate.c` exercises packet retention, duplicate retries,
unexpected packet rejection, `data_following`, zero-length termination,
timeout, and single-call abort behavior with a host mock. It does not verify the
SSC binary, the async core (not implemented in this stage), a real Flash driver,
a board, or a master.
