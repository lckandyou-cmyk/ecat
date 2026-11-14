#include "ecat_upgrade.h"
#include <string.h>

int ecat_upgrade_init(ecat_upgrade_t *u, ecat_foe_core_t *core)
{
    if (u == NULL || core == NULL || !core->is_configured ||
        core->state != ECAT_FOE_STATE_IDLE || core->session_active) return -1;
    memset(u, 0, sizeof(*u));
    u->core = core;
    return 0;
}

int ecat_upgrade_init_async(ecat_upgrade_t *u, ecat_foe_core_t *core,
    const ecat_upgrade_async_ops_t *ops, void *ctx)
{
    if (core == NULL || ops == NULL || ops->start == NULL || ops->poll == NULL ||
        ops->abort == NULL || ctx == NULL || core->async_abort_hook != ops->abort ||
        core->async_abort_ctx != ctx) return -1;
    if (ecat_upgrade_init(u, core) != 0) return -1;
    u->async_ops = ops;
    u->async_ctx = ctx;
    return 0;
}

static bool owns(ecat_upgrade_t *u, const void *owner, uint64_t token)
{
    return u != NULL && u->core != NULL && ecat_upgrade_session_owns(&u->session, owner, token);
}

static ecat_upgrade_result_t fail(ecat_upgrade_t *u, const void *owner, uint64_t token)
{
    u->last_error = ecat_foe_core_get_last_error(u->core);
    if (u->last_error == ECAT_FOE_CORE_OK) u->last_error = ECAT_FOE_CORE_ERR_INVALID_ARG;
    u->pending = false;
    u->ending = false;
    ecat_foe_core_on_abort(u->core);
    (void)ecat_upgrade_session_release(&u->session, owner, token);
    return ECAT_UPGRADE_FAILED;
}

ecat_upgrade_result_t ecat_upgrade_begin(ecat_upgrade_t *u, const void *owner,
    const ecat_upgrade_request_t *request, uint64_t *token)
{
    if (u == NULL || u->core == NULL || request == NULL || token == NULL || owner == NULL)
        return ECAT_UPGRADE_INVALID;
    uint64_t acquired = 0;
    ecat_upgrade_session_result_t result = ecat_upgrade_session_acquire(&u->session, owner, &acquired);
    if (result == ECAT_UPGRADE_SESSION_BUSY) return ECAT_UPGRADE_BUSY;
    if (result != ECAT_UPGRADE_SESSION_OK) return ECAT_UPGRADE_FAILED;
    if (request->target_id < 1U || request->target_id > ECAT_FOE_TARGET_COUNT ||
        !u->core->cfg.targets[request->target_id - 1U].enabled ||
        (u->core->cfg.targets[request->target_id - 1U].asynchronous && u->async_ops == NULL)) {
        u->last_error = ECAT_FOE_CORE_ERR_CFG_TARGET;
        (void)ecat_upgrade_session_release(&u->session, owner, acquired);
        return ECAT_UPGRADE_FAILED;
    }
    if (u->core->state == ECAT_FOE_STATE_SUCCESS || u->core->state == ECAT_FOE_STATE_ERROR)
        ecat_foe_core_on_abort(u->core);
    u->last_error = ecat_foe_core_begin_request(u->core, request);
    if (u->last_error != ECAT_FOE_CORE_OK) {
        (void)ecat_upgrade_session_release(&u->session, owner, acquired);
        return ECAT_UPGRADE_FAILED;
    }
    u->asynchronous = u->core->cfg.targets[request->target_id - 1U].asynchronous;
    u->pending = false;
    u->ending = false;
    *token = acquired;
    return ECAT_UPGRADE_COMPLETE;
}

static ecat_upgrade_result_t async_result(ecat_upgrade_t *u, const void *owner,
    uint64_t token, ecat_upgrade_async_result_t result)
{
    if (result == ECAT_UPGRADE_ASYNC_FAILED) return fail(u, owner, token);
    u->pending = result == ECAT_UPGRADE_ASYNC_PENDING;
    if (u->pending) return ECAT_UPGRADE_PENDING;
    if (result != ECAT_UPGRADE_ASYNC_COMPLETE) return fail(u, owner, token);
    if (u->ending) {
        if (u->core->state != ECAT_FOE_STATE_SUCCESS) return fail(u, owner, token);
        u->ending = false;
        u->last_error = ECAT_FOE_CORE_OK;
        (void)ecat_upgrade_session_release(&u->session, owner, token);
    }
    return ECAT_UPGRADE_COMPLETE;
}

ecat_upgrade_result_t ecat_upgrade_write(ecat_upgrade_t *u, const void *owner,
    uint64_t token, const uint8_t *data, size_t length)
{
    if (!owns(u, owner, token)) return ECAT_UPGRADE_STALE;
    if (u->pending) return ECAT_UPGRADE_BUSY;
    if (length == 0U) return ECAT_UPGRADE_COMPLETE;
    if (u->asynchronous)
        return async_result(u, owner, token, u->async_ops->start(u->async_ctx, data, length, true));
    if (ecat_foe_core_on_data(u->core, data, length) != 0) return fail(u, owner, token);
    return ECAT_UPGRADE_COMPLETE;
}

ecat_upgrade_result_t ecat_upgrade_finish(ecat_upgrade_t *u, const void *owner, uint64_t token)
{
    if (!owns(u, owner, token)) return ECAT_UPGRADE_STALE;
    if (u->pending) return ECAT_UPGRADE_BUSY;
    if (u->asynchronous) {
        u->ending = true;
        return async_result(u, owner, token, u->async_ops->start(u->async_ctx, NULL, 0U, false));
    }
    if (ecat_foe_core_on_end(u->core) != 0) return fail(u, owner, token);
    u->last_error = ECAT_FOE_CORE_OK;
    (void)ecat_upgrade_session_release(&u->session, owner, token);
    return ECAT_UPGRADE_COMPLETE;
}

ecat_upgrade_result_t ecat_upgrade_poll(ecat_upgrade_t *u, const void *owner, uint64_t token)
{
    if (!owns(u, owner, token)) return ECAT_UPGRADE_STALE;
    if (!u->pending || u->async_ops == NULL) return ECAT_UPGRADE_INVALID;
    return async_result(u, owner, token, u->async_ops->poll(u->async_ctx));
}

ecat_upgrade_result_t ecat_upgrade_cancel(ecat_upgrade_t *u, const void *owner, uint64_t token)
{
    if (!owns(u, owner, token)) return ECAT_UPGRADE_STALE;
    ecat_foe_core_on_abort(u->core);
    u->pending = false;
    u->ending = false;
    (void)ecat_upgrade_session_release(&u->session, owner, token);
    return ECAT_UPGRADE_COMPLETE;
}
