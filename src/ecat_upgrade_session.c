#include "ecat_upgrade_session_internal.h"
#include <stddef.h>

ecat_upgrade_session_result_t ecat_upgrade_session_acquire_mgr(
    ecat_upgrade_session_t *session, const void *manager, const void *owner, uint64_t *token)
{
    if (session == NULL || manager == NULL || owner == NULL || token == NULL)
        return ECAT_UPGRADE_SESSION_INVALID;
    if (session->owner != NULL) return ECAT_UPGRADE_SESSION_BUSY;
    if (session->generation == UINT64_MAX) return ECAT_UPGRADE_SESSION_EXHAUSTED;
    session->generation++;
    session->owner = owner;
    session->manager = manager;
    *token = session->generation;
    return ECAT_UPGRADE_SESSION_OK;
}

bool ecat_upgrade_session_owns_mgr(
    const ecat_upgrade_session_t *session, const void *manager, const void *owner, uint64_t token)
{
    return session != NULL && manager != NULL && owner != NULL && token != 0U &&
           session->owner == owner && session->generation == token &&
           session->manager == manager;
}

ecat_upgrade_session_result_t ecat_upgrade_session_release_mgr(
    ecat_upgrade_session_t *session, const void *manager, const void *owner, uint64_t token)
{
    if (session == NULL || manager == NULL || owner == NULL || token == 0U) {
        return ECAT_UPGRADE_SESSION_INVALID;
    }
    if (!ecat_upgrade_session_owns_mgr(session, manager, owner, token)) {
        return ECAT_UPGRADE_SESSION_STALE;
    }
    session->owner = NULL;
    session->manager = NULL;
    return ECAT_UPGRADE_SESSION_OK;
}
