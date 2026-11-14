#include "ecat_foe_transport.h"
#include <string.h>

int ecat_foe_transport_init(ecat_foe_transport_t *t, ecat_upgrade_t *upgrade)
{
    if (t == NULL || upgrade == NULL || upgrade->core == NULL) return -1;
    memset(t, 0, sizeof(*t));
    t->upgrade = upgrade;
    return 0;
}

bool ecat_foe_transport_owns(const ecat_foe_transport_t *t)
{
    return t != NULL && t->upgrade != NULL &&
        ecat_upgrade_session_owns(&t->upgrade->session, t, t->token);
}

void ecat_foe_transport_abort(void *ctx)
{
    ecat_foe_transport_t *t = ctx;
    if (t == NULL) return;
    if (ecat_foe_transport_owns(t))
        (void)ecat_upgrade_cancel(t->upgrade, t, t->token);
    t->pending = false;
    t->finish_after_write = false;
    t->finishing = false;
}

int ecat_foe_transport_begin(ecat_foe_transport_t *t, const char *name, size_t length)
{
    if (t == NULL || t->upgrade == NULL || name == NULL) return ECAT_FOE_ERRCODE_ILLEGAL;
    if (t->upgrade->session.owner != NULL && !ecat_foe_transport_owns(t))
        return ECAT_FOE_ERRCODE_NOTDEFINED;
    ecat_foe_transport_abort(t);
    ecat_upgrade_request_t request;
    int result = ecat_foe_core_validate_wrq(t->upgrade->core, name, length,
                                            &request.target_id, &request.fw_version);
    if (result != 0) return result;
    return ecat_upgrade_begin(t->upgrade, t, &request, &t->token) == ECAT_UPGRADE_COMPLETE
        ? 0 : ECAT_FOE_ERRCODE_NOTDEFINED;
}

static ecat_upgrade_async_result_t advance(ecat_foe_transport_t *t, ecat_upgrade_result_t result)
{
    if (result == ECAT_UPGRADE_PENDING) {
        t->pending = true;
        return ECAT_UPGRADE_ASYNC_PENDING;
    }
    t->pending = false;
    if (result != ECAT_UPGRADE_COMPLETE) {
        ecat_foe_transport_abort(t);
        return ECAT_UPGRADE_ASYNC_FAILED;
    }
    if (t->finish_after_write && !t->finishing) {
        t->finishing = true;
        return advance(t, ecat_upgrade_finish(t->upgrade, t, t->token));
    }
    return ECAT_UPGRADE_ASYNC_COMPLETE;
}

ecat_upgrade_async_result_t ecat_foe_transport_start(void *ctx, const uint8_t *data,
    size_t length, bool more_data)
{
    ecat_foe_transport_t *t = ctx;
    if (!ecat_foe_transport_owns(t) || t->pending) return ECAT_UPGRADE_ASYNC_FAILED;
    t->finish_after_write = !more_data;
    t->finishing = false;
    return advance(t, ecat_upgrade_write(t->upgrade, t, t->token, data, length));
}

ecat_upgrade_async_result_t ecat_foe_transport_poll(void *ctx)
{
    ecat_foe_transport_t *t = ctx;
    if (t == NULL || !t->pending || !ecat_foe_transport_owns(t)) return ECAT_UPGRADE_ASYNC_FAILED;
    return advance(t, ecat_upgrade_poll(t->upgrade, t, t->token));
}

const ecat_upgrade_async_ops_t *ecat_foe_transport_async_ops(void)
{
    static const ecat_upgrade_async_ops_t ops = {
        ecat_foe_transport_start, ecat_foe_transport_poll, ecat_foe_transport_abort
    };
    return &ops;
}
