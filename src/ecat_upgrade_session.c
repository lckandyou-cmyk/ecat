#include "ecat_upgrade_session.h"
#include <stddef.h>

static ecat_upgrade_session_result_t session_acquire_internal(
    ecat_upgrade_session_t *session, const void *manager, const void *owner, uint64_t *token)
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
    session->manager = manager;
    *token = session->generation;
    return ECAT_UPGRADE_SESSION_OK;
}

ecat_upgrade_session_result_t ecat_upgrade_session_acquire_mgr(
    ecat_upgrade_session_t *session, const void *manager, const void *owner, uint64_t *token)
{
    if (manager == NULL) {
        return ECAT_UPGRADE_SESSION_INVALID;
    }
    return session_acquire_internal(session, manager, owner, token);
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

ecat_upgrade_session_result_t ecat_upgrade_session_acquire(
    ecat_upgrade_session_t *session, const void *owner, uint64_t *token)
{
    return session_acquire_internal(session, NULL, owner, token);
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
    if (session->manager != NULL) {
        /* manager-bound 租约禁止通过旧接口释放，必须由配套的管理器调用 release_mgr */
        return ECAT_UPGRADE_SESSION_STALE;
    }
    if (!ecat_upgrade_session_owns(session, owner, token)) {
        return ECAT_UPGRADE_SESSION_STALE;
    }
    session->owner = NULL;
    session->manager = NULL;
    return ECAT_UPGRADE_SESSION_OK;
}
