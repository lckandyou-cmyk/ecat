/** Protocol-independent, allocation-free exclusive upgrade ownership. */
#ifndef ECAT_UPGRADE_SESSION_H
#define ECAT_UPGRADE_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ECAT_UPGRADE_SESSION_OK = 0,
    ECAT_UPGRADE_SESSION_INVALID,
    ECAT_UPGRADE_SESSION_BUSY,
    ECAT_UPGRADE_SESSION_STALE,
    ECAT_UPGRADE_SESSION_EXHAUSTED
} ecat_upgrade_session_result_t;

/* Zero-initialize once before use; do not copy or reset while callers can hold
 * tokens. All calls must be serialized. owner and manager are identities only, never
 * dereferenced, and must remain unique for the lifetime of the session. */
typedef struct {
    const void *owner;
    uint64_t generation;
    const void *manager; /**< 绑定持有当前租约的具体管理器实例 (ecat_transfer_t) */
} ecat_upgrade_session_t;

/* Failure preserves both the manager and the caller's token output.
 * Note on legacy session APIs:
 * - ecat_upgrade_session_acquire operates on unmanaged leases (sets session->manager = NULL).
 * - ecat_upgrade_session_owns provides backward-compatible read-only observation of caller
 *   (owner, token) pairing, regardless of whether the lease is manager-bound; it does NOT
 *   verify manager identity.
 * - ecat_upgrade_session_release is restricted to unmanaged leases and strictly refuses to release
 *   manager-bound leases (returning ECAT_UPGRADE_SESSION_STALE). */
ecat_upgrade_session_result_t ecat_upgrade_session_acquire(
    ecat_upgrade_session_t *session, const void *owner, uint64_t *token);
bool ecat_upgrade_session_owns(
    const ecat_upgrade_session_t *session, const void *owner, uint64_t token);
ecat_upgrade_session_result_t ecat_upgrade_session_release(
    ecat_upgrade_session_t *session, const void *owner, uint64_t token);

/* Manager-bound session APIs: bind lease to both caller owner and specific manager instance.
 * - ecat_upgrade_session_acquire_mgr requires manager != NULL (returns INVALID on NULL).
 * - ecat_upgrade_session_owns_mgr and release_mgr strictly require manager matching session->manager.
 * - Legacy ecat_upgrade_session_release cannot release manager-bound leases (returns STALE).
 * - Legacy ecat_upgrade_session_owns may still be used for read-only observation of (owner, token),
 *   but manager-bound authorization requires ecat_upgrade_session_owns_mgr. */
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
