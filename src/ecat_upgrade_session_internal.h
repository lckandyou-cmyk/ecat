#ifndef ECAT_UPGRADE_SESSION_INTERNAL_H
#define ECAT_UPGRADE_SESSION_INTERNAL_H

#include "ecat_upgrade_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every lease binds manager, owner and token. Failed operations preserve state. */
ecat_upgrade_session_result_t ecat_upgrade_session_acquire_mgr(
    ecat_upgrade_session_t *session, const void *manager, const void *owner, uint64_t *token);
bool ecat_upgrade_session_owns_mgr(
    const ecat_upgrade_session_t *session, const void *manager, const void *owner, uint64_t token);
ecat_upgrade_session_result_t ecat_upgrade_session_release_mgr(
    ecat_upgrade_session_t *session, const void *manager, const void *owner, uint64_t token);

#ifdef __cplusplus
}
#endif
#endif
