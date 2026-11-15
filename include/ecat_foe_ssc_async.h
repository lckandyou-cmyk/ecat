/**
 * @file ecat_foe_ssc_async.h
 * @brief Single-flight SSC FoE DATA/BUSY retry gate.
 *
 * This small, allocation-free adapter primitive owns one DATA packet while a
 * caller-driven asynchronous operation is pending. It does not implement the
 * ECAT core or a storage driver; applications must connect the callbacks to a
 * stateful asynchronous core before using it for firmware updates.
 *
 * Lifecycle and hardware synchronization contract:
 * 1. Buffer overlap safety:
 *    - If caller input data equals packet_storage, redundant copies are skipped;
 *    - If data and packet_storage point to overlapping offsets within the same
 *      memory array, safe memmove is used, eliminating memcpy undefined behavior.
 * 2. Hardware synchronous abort requirement:
 *    - ops->abort(ctx) MUST synchronously terminate all hardware (DMA, bus)
 *      access to packet_storage before returning. Drivers unable to stop hardware
 *      synchronously must not use this shared contract without double buffering.
 * 3. Status transitions and clear_packet timing:
 *    - COMPLETE: Hardware I/O has completely ceased. packet_storage is safely
 *      zeroed via clear_packet once handshake closes or on final ACK.
 *    - FAILED / TIMEOUT: ops->abort(ctx) is synchronously called first to stop
 *      hardware. packet_storage retains payload for master packet-match retry
 *      comparison and is zeroed upon retry completion or protocol termination.
 *    - Cancel / duplicate WRQ: ops->abort(ctx) stops hardware before clear_packet.
 */
#ifndef ECAT_FOE_SSC_ASYNC_H
#define ECAT_FOE_SSC_ASYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ecat_foe_async_compat.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef struct {
    const ecat_foe_ssc_async_ops_t *ops;
    void *ops_ctx;
    uint8_t *packet_storage;       /* Caller-owned static storage. */
    size_t packet_capacity;
    uint32_t timeout_ticks;
    uint32_t deadline;
    uint16_t busy_foe_code;
    uint16_t packet_error_foe_code;
    uint16_t operation_error_foe_code;
    bool pending;
    bool completed;
    bool following;
    bool operation_failed;
    size_t packet_length;
} ecat_foe_ssc_async_t;

/*
 * Initialize with caller-owned packet storage and SSC-specific FoE return
 * values. BUSY must differ from both error values. `ops`, storage, and
 * context must outlive the adapter.
 */
int ecat_foe_ssc_async_init(ecat_foe_ssc_async_t *adapter,
                            const ecat_foe_ssc_async_ops_t *ops,
                            void *ops_ctx,
                            uint8_t *packet_storage,
                            size_t packet_capacity,
                            uint32_t timeout_ticks,
                            uint16_t busy_foe_code,
                            uint16_t packet_error_foe_code,
                            uint16_t operation_error_foe_code);

/* Called by the SSC DATA callback. It never waits for a pending operation. */
uint16_t ecat_foe_ssc_async_on_data(ecat_foe_ssc_async_t *adapter,
                                    const uint8_t *data,
                                    size_t length,
                                    bool data_following,
                                    uint32_t now_ticks);

/* Call regularly from the application main loop; wrap-safe for uint32 ticks. */
uint16_t ecat_foe_ssc_async_poll(ecat_foe_ssc_async_t *adapter,
                                 uint32_t now_ticks);

/* Cancel a pending operation and release its packet state. */
void ecat_foe_ssc_async_abort(ecat_foe_ssc_async_t *adapter);

/* Abort the active async core/target session even when no packet is pending. */
void ecat_foe_ssc_async_abort_session(ecat_foe_ssc_async_t *adapter);

#ifdef __cplusplus
}
#endif

#endif
