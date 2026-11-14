/** Owned synchronous byte-stream upgrade interface. */
#ifndef ECAT_UPGRADE_H
#define ECAT_UPGRADE_H

#include "ecat_foe_core.h"
#include "ecat_upgrade_session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ECAT_UPGRADE_COMPLETE = 0,
    ECAT_UPGRADE_PENDING,
    ECAT_UPGRADE_FAILED,
    ECAT_UPGRADE_BUSY,
    ECAT_UPGRADE_INVALID,
    ECAT_UPGRADE_STALE
} ecat_upgrade_result_t;

typedef struct {
    ecat_foe_core_t *core;
    ecat_upgrade_session_t session;
    ecat_foe_core_err_t last_error;
} ecat_upgrade_t;

/* Initialize once with an IDLE configured core. Afterwards this manager must
 * exclusively own the core: do not mix legacy event APIs or a second manager.
 * Calls are serialized; owners are stable identity pointers. */
int ecat_upgrade_init(ecat_upgrade_t *upgrade, ecat_foe_core_t *core);
ecat_upgrade_result_t ecat_upgrade_begin(ecat_upgrade_t *upgrade, const void *owner,
    const ecat_upgrade_request_t *request, uint64_t *token);
/* Synchronous: bytes are consumed before returning; zero bytes never end a
 * session. Errors from the current owner close the session and retain a local
 * diagnostic. Stale owner/token errors do not change any state. */
ecat_upgrade_result_t ecat_upgrade_write(ecat_upgrade_t *upgrade, const void *owner,
    uint64_t token, const uint8_t *data, size_t length);
ecat_upgrade_result_t ecat_upgrade_finish(ecat_upgrade_t *upgrade, const void *owner, uint64_t token);
ecat_upgrade_result_t ecat_upgrade_cancel(ecat_upgrade_t *upgrade, const void *owner, uint64_t token);

#ifdef __cplusplus
}
#endif
#endif
