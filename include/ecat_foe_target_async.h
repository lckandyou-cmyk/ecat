/**
 * @file ecat_foe_target_async.h
 * @brief Allocation-free single-operation async target-driver runner.
 *
 * This is the non-blocking target-operation primitive used by the async core
 * integration. It is additive: the existing synchronous target-driver API is
 * unchanged. The runner owns one fixed-size staging buffer and never queues a
 * second operation while one is pending.
 */
#ifndef ECAT_FOE_TARGET_ASYNC_H
#define ECAT_FOE_TARGET_ASYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ECAT_FOE_TARGET_ASYNC_FAILED = -1,
    ECAT_FOE_TARGET_ASYNC_COMPLETE = 0,
    ECAT_FOE_TARGET_ASYNC_PENDING = 1
} ecat_foe_target_async_result_t;

typedef enum {
    ECAT_FOE_TARGET_OP_INIT = 0,
    ECAT_FOE_TARGET_OP_INVALIDATE,
    ECAT_FOE_TARGET_OP_ERASE_TO,
    ECAT_FOE_TARGET_OP_WRITE,
    ECAT_FOE_TARGET_OP_READ,
    ECAT_FOE_TARGET_OP_FINALIZE
} ecat_foe_target_op_kind_t;

/* The descriptor and its data remain stable from start until completion. */
typedef struct {
    ecat_foe_target_op_kind_t kind;
    uint32_t offset;
    uint32_t need_end;
    uint32_t erased_end; /* Driver writes this for ERASE_TO. */
    uint32_t length;
    uint8_t *data;       /* WRITE input or READ output staging buffer. */
    uint16_t target_id;  /* FINALIZE metadata; ignored for other operations. */
    uint32_t fw_version;
    uint32_t min_allow_version;
    bool anti_rollback;
} ecat_foe_target_async_op_t;

typedef struct ecat_foe_target_async_ops {
    ecat_foe_target_async_result_t (*start)(void *ctx,
                                            ecat_foe_target_async_op_t *op);
    ecat_foe_target_async_result_t (*poll)(void *ctx,
                                           ecat_foe_target_async_op_t *op);
    void (*abort)(void *ctx);
    /* Release the active target session after an operation or protocol error. */
    void (*abort_session)(void *ctx);
} ecat_foe_target_async_ops_t;

typedef struct {
    const ecat_foe_target_async_ops_t *ops;
    void *ctx;
    uint8_t *storage;
    size_t storage_capacity;
    ecat_foe_target_async_op_t op;
    uint8_t *read_destination;
    uint32_t *erased_end_destination;
    bool pending;
    bool failed;
} ecat_foe_target_async_t;

int ecat_foe_target_async_init(ecat_foe_target_async_t *runner,
                               const ecat_foe_target_async_ops_t *ops,
                               void *ctx,
                               uint8_t *storage,
                               size_t storage_capacity);

/* Start one operation. WRITE bytes are copied into runner-owned storage.
 * READ destination and ERASE_TO output pointers must remain valid until done.
 */
ecat_foe_target_async_result_t ecat_foe_target_async_start(
    ecat_foe_target_async_t *runner,
    ecat_foe_target_op_kind_t kind,
    uint32_t offset,
    uint32_t need_end,
    const uint8_t *data,
    uint32_t length,
    uint8_t *read_destination,
    uint32_t *erased_end_destination);

ecat_foe_target_async_result_t ecat_foe_target_async_start_finalize(
    ecat_foe_target_async_t *runner,
    uint16_t target_id,
    uint32_t fw_version,
    uint32_t min_allow_version,
    bool anti_rollback);

ecat_foe_target_async_result_t ecat_foe_target_async_poll(
    ecat_foe_target_async_t *runner);

/* Cancel a pending operation at most once; completed operations are untouched. */
void ecat_foe_target_async_abort(ecat_foe_target_async_t *runner);

/* Abort the current operation if pending and release its target session. */
void ecat_foe_target_async_abort_session(ecat_foe_target_async_t *runner);

/* Prepare a failed/aborted runner for a new session. Pending work must first
 * be cancelled; reset never silently abandons an in-flight driver operation.
 */
int ecat_foe_target_async_reset(ecat_foe_target_async_t *runner);

#ifdef __cplusplus
}
#endif
#endif
