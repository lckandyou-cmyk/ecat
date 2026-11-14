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

static bool owns(ecat_upgrade_t *u, const void *owner, uint64_t token)
{
    return u != NULL && u->core != NULL && ecat_upgrade_session_owns(&u->session, owner, token);
}

static ecat_upgrade_result_t fail(ecat_upgrade_t *u, const void *owner, uint64_t token)
{
    u->last_error = ecat_foe_core_get_last_error(u->core);
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
        u->core->cfg.targets[request->target_id - 1U].asynchronous) {
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
    *token = acquired;
    return ECAT_UPGRADE_COMPLETE;
}

ecat_upgrade_result_t ecat_upgrade_write(ecat_upgrade_t *u, const void *owner,
    uint64_t token, const uint8_t *data, size_t length)
{
    if (!owns(u, owner, token)) return ECAT_UPGRADE_STALE;
    if (length == 0U) return ECAT_UPGRADE_COMPLETE;
    if (ecat_foe_core_on_data(u->core, data, length) != 0) return fail(u, owner, token);
    return ECAT_UPGRADE_COMPLETE;
}

ecat_upgrade_result_t ecat_upgrade_finish(ecat_upgrade_t *u, const void *owner, uint64_t token)
{
    if (!owns(u, owner, token)) return ECAT_UPGRADE_STALE;
    if (ecat_foe_core_on_end(u->core) != 0) return fail(u, owner, token);
    u->last_error = ECAT_FOE_CORE_OK;
    (void)ecat_upgrade_session_release(&u->session, owner, token);
    return ECAT_UPGRADE_COMPLETE;
}

ecat_upgrade_result_t ecat_upgrade_cancel(ecat_upgrade_t *u, const void *owner, uint64_t token)
{
    if (!owns(u, owner, token)) return ECAT_UPGRADE_STALE;
    ecat_foe_core_on_abort(u->core);
    (void)ecat_upgrade_session_release(&u->session, owner, token);
    return ECAT_UPGRADE_COMPLETE;
}
