/* Test-only adapter for exercising firmware policy without a protocol stack. */
#ifndef TEST_CORE_HELPERS_H
#define TEST_CORE_HELPERS_H
#include "../src/ecat_foe_core_internal.h"

static int test_core_begin_filename(ecat_foe_core_t *core, const char *name, size_t length)
{
    uint16_t target = 0;
    uint32_t version = 0;
    if (core == NULL || name == NULL || !core->is_configured)
        return ECAT_FOE_ERRCODE_NOTDEFINED;
    if (core->state != ECAT_FOE_STATE_IDLE || core->session_active)
        ecat_foe_core_on_abort(core);
    int rc = ecat_foe_core_validate_wrq(core, name, length, &target, &version);
    if (rc != 0) return rc;
    ecat_upgrade_request_t request = {target, version};
    return ecat_foe_core_err_to_foe(ecat_foe_core_begin_request(core, &request), true, false);
}
#endif
