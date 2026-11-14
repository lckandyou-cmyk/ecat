/* This public engine header must compile without pulling in the SSC gate. */
#include "ecat_foe_core_async.h"
#ifdef ECAT_FOE_SSC_ASYNC_H
#error "Asynchronous upgrade engine must not include the SSC adapter"
#endif
const ecat_upgrade_async_ops_t *test_upgrade_async_header(void);
const ecat_upgrade_async_ops_t *test_upgrade_async_header(void)
{
    return ecat_foe_core_async_ops();
}
