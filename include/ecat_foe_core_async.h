/** Allocation-free asynchronous FoE session engine. */
#ifndef ECAT_FOE_CORE_ASYNC_H
#define ECAT_FOE_CORE_ASYNC_H

#include "ecat_foe_core.h"
#include "ecat_foe_target_async.h"
#include "ecat_upgrade_async.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ECAT_FOE_CORE_ASYNC_IDLE = 0,
    ECAT_FOE_CORE_ASYNC_PACKET,
    ECAT_FOE_CORE_ASYNC_ENDING,
    ECAT_FOE_CORE_ASYNC_FAILED
} ecat_foe_core_async_state_t;

typedef enum {
    ECAT_FOE_CORE_ASYNC_PHASE_NONE = 0,
    ECAT_FOE_CORE_ASYNC_PHASE_INIT,
    ECAT_FOE_CORE_ASYNC_PHASE_INVALIDATE,
    ECAT_FOE_CORE_ASYNC_PHASE_ERASE,
    ECAT_FOE_CORE_ASYNC_PHASE_WRITE,
    ECAT_FOE_CORE_ASYNC_PHASE_READ,
    ECAT_FOE_CORE_ASYNC_PHASE_FINALIZE
} ecat_foe_core_async_phase_t;

/* Buffers are caller-owned, distinct static storage. packet_capacity must
 * cover one SSC DATA payload; operation_capacity must cover the largest
 * write_unit and is at least 64 bytes for readback staging. The session/core
 * pair is non-copyable after binding. */
typedef struct {
    ecat_foe_core_t *core;
    ecat_foe_target_async_t runner;
    uint8_t *packet;
    size_t packet_capacity;
    uint8_t *operation_storage;
    size_t operation_capacity;
    ecat_foe_core_async_state_t state;
    ecat_foe_core_async_phase_t phase;
    size_t packet_length;
    size_t packet_cursor;
    size_t packet_payload_start;
    bool packet_following;
    bool packet_complete;
    bool packet_accounted;
    bool end_requested;
    bool target_session_started;
    bool target_session_aborted;
    uint32_t operation_offset;
    uint32_t operation_length;
    uint32_t operation_need_end;
    uint32_t readback_crc;
    uint32_t readback_offset;
    uint32_t readback_remaining;
    uint32_t readback_chunk;
    uint8_t readback[64];
} ecat_foe_core_async_t;

int ecat_foe_core_async_init(ecat_foe_core_async_t *session,
                             ecat_foe_core_t *core,
                             uint8_t *packet_storage,
                             size_t packet_capacity,
                             uint8_t *operation_storage,
                             size_t operation_capacity);

/* Compatible with ecat_foe_ssc_async_ops_t; bDataFollowing is carried through. */
const ecat_upgrade_async_ops_t *ecat_foe_core_async_ops(void);

/* Legacy getter, same backend and contract; retained for existing SSC users. */
const ecat_upgrade_async_ops_t *ecat_foe_core_async_ssc_ops(void);

/* Use these as the SSC gate's callbacks, with session as ops_ctx. */
void ecat_foe_core_async_abort(ecat_foe_core_async_t *session);

#ifdef __cplusplus
}
#endif
#endif
