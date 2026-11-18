#ifndef ECAT_FOE_TARGET_ASYNC_INTERNAL_H
#define ECAT_FOE_TARGET_ASYNC_INTERNAL_H

#include "ecat_foe_target_async.h"

#ifdef __cplusplus
extern "C" {
#endif

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

/* Abort the current operation if pending and forward session abort to ops->abort_session.
 * Calls ops->abort at most once (while pending); forwarding to ops->abort_session occurs
 * per invocation. Upper-level session management (such as ecat_foe_core_async) enforces
 * session lifecycle and guarantees that abort_session is invoked at most once per session.
 */
void ecat_foe_target_async_abort_session(ecat_foe_target_async_t *runner);

/* Prepare a failed/aborted runner for a new session. Pending work must first
 * be cancelled; reset never silently abandons an in-flight driver operation.
 */
int ecat_foe_target_async_reset(ecat_foe_target_async_t *runner);

#ifdef __cplusplus
}
#endif
#endif
