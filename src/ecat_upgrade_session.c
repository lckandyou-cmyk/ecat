#include "ecat_upgrade_session.h"
#include <stddef.h>

ecat_upgrade_session_result_t ecat_upgrade_session_acquire(
    ecat_upgrade_session_t *session, const void *owner, uint64_t *token)
{
    if (session == NULL || owner == NULL || token == NULL) {
        return ECAT_UPGRADE_SESSION_INVALID;
    }
    if (session->owner != NULL) {
        return ECAT_UPGRADE_SESSION_BUSY;
    }
    if (session->generation == UINT64_MAX) {
        return ECAT_UPGRADE_SESSION_EXHAUSTED;
    }
    session->generation++;
    session->owner = owner;
    *token = session->generation;
    return ECAT_UPGRADE_SESSION_OK;
}

bool ecat_upgrade_session_owns(
    const ecat_upgrade_session_t *session, const void *owner, uint64_t token)
{
    return session != NULL && owner != NULL && token != 0U &&
           session->owner == owner && session->generation == token;
}

ecat_upgrade_session_result_t ecat_upgrade_session_release(
    ecat_upgrade_session_t *session, const void *owner, uint64_t token)
{
    if (session == NULL || owner == NULL || token == 0U) {
        return ECAT_UPGRADE_SESSION_INVALID;
    }
    if (!ecat_upgrade_session_owns(session, owner, token)) {
        return ECAT_UPGRADE_SESSION_STALE;
    }
    session->owner = NULL;
    return ECAT_UPGRADE_SESSION_OK;
}
