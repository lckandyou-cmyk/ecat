/** Legacy asynchronous backend names; no SSC gate dependency. */
#ifndef ECAT_FOE_ASYNC_COMPAT_H
#define ECAT_FOE_ASYNC_COMPAT_H

#include "ecat_upgrade_async.h"

typedef ecat_upgrade_async_result_t ecat_foe_ssc_async_result_t;
typedef ecat_upgrade_async_ops_t ecat_foe_ssc_async_ops_t;
#define ECAT_FOE_SSC_ASYNC_FAILED ECAT_UPGRADE_ASYNC_FAILED
#define ECAT_FOE_SSC_ASYNC_COMPLETE ECAT_UPGRADE_ASYNC_COMPLETE
#define ECAT_FOE_SSC_ASYNC_PENDING ECAT_UPGRADE_ASYNC_PENDING

#endif
