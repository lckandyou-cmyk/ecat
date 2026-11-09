#include "ecat_foe_target_async.h"

#include <string.h>

static void clear_operation(ecat_foe_target_async_t *runner)
{
    runner->pending = false;
    runner->op.kind = ECAT_FOE_TARGET_OP_INIT;
    runner->op.offset = 0U;
    runner->op.need_end = 0U;
    runner->op.erased_end = 0U;
    runner->op.length = 0U;
    runner->op.data = NULL;
    runner->op.target_id = 0U;
    runner->op.fw_version = 0U;
    runner->op.min_allow_version = 0U;
    runner->op.anti_rollback = false;
    runner->read_destination = NULL;
    runner->erased_end_destination = NULL;
}

static void finish_operation(ecat_foe_target_async_t *runner,
                             ecat_foe_target_async_result_t result)
{
    if (result == ECAT_FOE_TARGET_ASYNC_COMPLETE) {
        if (runner->op.kind == ECAT_FOE_TARGET_OP_READ && runner->op.length > 0U) {
            memcpy(runner->read_destination, runner->storage, runner->op.length);
        } else if (runner->op.kind == ECAT_FOE_TARGET_OP_ERASE_TO) {
            *runner->erased_end_destination = runner->op.erased_end;
        }
        runner->failed = false;
    } else {
        runner->failed = true;
    }
    clear_operation(runner);
}

int ecat_foe_target_async_init(ecat_foe_target_async_t *runner,
                               const ecat_foe_target_async_ops_t *ops,
                               void *ctx,
                               uint8_t *storage,
                               size_t storage_capacity)
{
    if (runner == NULL || ops == NULL || ops->start == NULL || ops->poll == NULL ||
        ops->abort == NULL || storage == NULL || storage_capacity == 0U) {
        return -1;
    }

    memset(runner, 0, sizeof(*runner));
    runner->ops = ops;
    runner->ctx = ctx;
    runner->storage = storage;
    runner->storage_capacity = storage_capacity;
    return 0;
}

ecat_foe_target_async_result_t ecat_foe_target_async_start(
    ecat_foe_target_async_t *runner,
    ecat_foe_target_op_kind_t kind,
    uint32_t offset,
    uint32_t need_end,
    const uint8_t *data,
    uint32_t length,
    uint8_t *read_destination,
    uint32_t *erased_end_destination)
{
    if (runner == NULL || runner->ops == NULL || runner->pending || runner->failed ||
        kind < ECAT_FOE_TARGET_OP_INIT || kind > ECAT_FOE_TARGET_OP_FINALIZE) {
        return ECAT_FOE_TARGET_ASYNC_FAILED;
    }
    if (kind == ECAT_FOE_TARGET_OP_WRITE &&
        (data == NULL || length == 0U || (size_t)length > runner->storage_capacity)) {
        return ECAT_FOE_TARGET_ASYNC_FAILED;
    }
    if (kind == ECAT_FOE_TARGET_OP_READ &&
        ((size_t)length > runner->storage_capacity ||
         (length > 0U && read_destination == NULL))) {
        return ECAT_FOE_TARGET_ASYNC_FAILED;
    }
    if (kind == ECAT_FOE_TARGET_OP_ERASE_TO && erased_end_destination == NULL) {
        return ECAT_FOE_TARGET_ASYNC_FAILED;
    }
    if ((kind == ECAT_FOE_TARGET_OP_INIT || kind == ECAT_FOE_TARGET_OP_INVALIDATE ||
         kind == ECAT_FOE_TARGET_OP_FINALIZE) && length != 0U) {
        return ECAT_FOE_TARGET_ASYNC_FAILED;
    }

    runner->op.kind = kind;
    runner->op.offset = offset;
    runner->op.need_end = need_end;
    runner->op.erased_end = 0U;
    runner->op.length = length;
    runner->op.target_id = 0U;
    runner->op.fw_version = 0U;
    runner->op.min_allow_version = 0U;
    runner->op.anti_rollback = false;
    runner->read_destination = read_destination;
    runner->erased_end_destination = erased_end_destination;
    if (kind == ECAT_FOE_TARGET_OP_WRITE) {
        if (runner->storage != data) {
            memcpy(runner->storage, data, length);
        }
        runner->op.data = runner->storage;
    } else if (kind == ECAT_FOE_TARGET_OP_READ) {
        runner->op.data = runner->storage;
    } else {
        runner->op.data = NULL;
    }

    ecat_foe_target_async_result_t result = runner->ops->start(runner->ctx, &runner->op);
    if (result == ECAT_FOE_TARGET_ASYNC_PENDING) {
        runner->pending = true;
    } else {
        finish_operation(runner, result);
    }
    return result;
}

ecat_foe_target_async_result_t ecat_foe_target_async_poll(
    ecat_foe_target_async_t *runner)
{
    if (runner == NULL || runner->ops == NULL || !runner->pending) {
        return ECAT_FOE_TARGET_ASYNC_FAILED;
    }
    ecat_foe_target_async_result_t result = runner->ops->poll(runner->ctx, &runner->op);
    if (result != ECAT_FOE_TARGET_ASYNC_PENDING) {
        runner->pending = false;
        finish_operation(runner, result);
    }
    return result;
}

ecat_foe_target_async_result_t ecat_foe_target_async_start_finalize(
    ecat_foe_target_async_t *runner,
    uint16_t target_id,
    uint32_t fw_version,
    uint32_t min_allow_version,
    bool anti_rollback)
{
    if (runner == NULL || runner->ops == NULL || runner->pending || runner->failed ||
        target_id == 0U) {
        return ECAT_FOE_TARGET_ASYNC_FAILED;
    }
    memset(&runner->op, 0, sizeof(runner->op));
    runner->op.kind = ECAT_FOE_TARGET_OP_FINALIZE;
    runner->op.target_id = target_id;
    runner->op.fw_version = fw_version;
    runner->op.min_allow_version = min_allow_version;
    runner->op.anti_rollback = anti_rollback;
    ecat_foe_target_async_result_t result = runner->ops->start(runner->ctx, &runner->op);
    runner->pending = result == ECAT_FOE_TARGET_ASYNC_PENDING;
    if (!runner->pending) {
        finish_operation(runner, result);
    }
    return result;
}

void ecat_foe_target_async_abort(ecat_foe_target_async_t *runner)
{
    if (runner == NULL || runner->ops == NULL || !runner->pending) {
        return;
    }
    runner->ops->abort(runner->ctx);
    clear_operation(runner);
    runner->failed = true;
}

void ecat_foe_target_async_abort_session(ecat_foe_target_async_t *runner)
{
    if (runner == NULL || runner->ops == NULL) {
        return;
    }
    if (runner->pending) {
        runner->ops->abort(runner->ctx);
        clear_operation(runner);
    }
    if (runner->ops->abort_session != NULL) {
        runner->ops->abort_session(runner->ctx);
    }
    runner->failed = true;
}

int ecat_foe_target_async_reset(ecat_foe_target_async_t *runner)
{
    if (runner == NULL || runner->ops == NULL || runner->pending) {
        return -1;
    }
    clear_operation(runner);
    runner->failed = false;
    return 0;
}
