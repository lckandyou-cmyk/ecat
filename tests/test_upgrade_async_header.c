/* This public engine header must compile without pulling in the SSC gate. */
#include "ecat_foe_core_async.h"
#ifdef ECAT_FOE_SSC_ASYNC_H
#error "Asynchronous upgrade engine must not include the SSC adapter"
#endif
ecat_foe_core_async_t *test_upgrade_async_header(ecat_foe_core_async_t *session);
ecat_foe_core_async_t *test_upgrade_async_header(ecat_foe_core_async_t *session)
{
    return session;
}
