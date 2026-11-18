#include "../src/ecat_upgrade_session_internal.h"
#include <stdint.h>
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    ecat_upgrade_session_t session = {0};
    int mgr1 = 1, mgr2 = 2, foe = 0, file = 0;
    uint64_t first = 0, second = 0, rejected = 99;
    CHECK(ecat_upgrade_session_acquire_mgr(NULL, &mgr1, &foe, &first) == ECAT_UPGRADE_SESSION_INVALID);
    CHECK(ecat_upgrade_session_acquire_mgr(&session, NULL, &foe, &first) == ECAT_UPGRADE_SESSION_INVALID);
    CHECK(ecat_upgrade_session_acquire_mgr(&session, &mgr1, NULL, &first) == ECAT_UPGRADE_SESSION_INVALID);
    CHECK(ecat_upgrade_session_acquire_mgr(&session, &mgr1, &foe, NULL) == ECAT_UPGRADE_SESSION_INVALID);
    CHECK(ecat_upgrade_session_acquire_mgr(&session, &mgr1, &foe, &first) == ECAT_UPGRADE_SESSION_OK);
    CHECK(first != 0 && ecat_upgrade_session_owns_mgr(&session, &mgr1, &foe, first));
    CHECK(ecat_upgrade_session_acquire_mgr(&session, &mgr2, &file, &rejected) == ECAT_UPGRADE_SESSION_BUSY);
    CHECK(rejected == 99 && ecat_upgrade_session_owns_mgr(&session, &mgr1, &foe, first));
    CHECK(!ecat_upgrade_session_owns_mgr(&session, &mgr2, &foe, first));
    CHECK(!ecat_upgrade_session_owns_mgr(&session, NULL, &foe, first));
    CHECK(ecat_upgrade_session_release_mgr(&session, &mgr2, &foe, first) == ECAT_UPGRADE_SESSION_STALE);
    CHECK(ecat_upgrade_session_release_mgr(&session, NULL, &foe, first) == ECAT_UPGRADE_SESSION_INVALID);
    CHECK(ecat_upgrade_session_release_mgr(&session, &mgr1, &file, first) == ECAT_UPGRADE_SESSION_STALE);
    CHECK(ecat_upgrade_session_release_mgr(&session, &mgr1, &foe, first + 1U) == ECAT_UPGRADE_SESSION_STALE);
    CHECK(ecat_upgrade_session_release_mgr(&session, &mgr1, &foe, first) == ECAT_UPGRADE_SESSION_OK);
    CHECK(ecat_upgrade_session_release_mgr(&session, &mgr1, &foe, first) == ECAT_UPGRADE_SESSION_STALE);
    CHECK(ecat_upgrade_session_acquire_mgr(&session, &mgr2, &file, &second) == ECAT_UPGRADE_SESSION_OK);
    CHECK(second > first && !ecat_upgrade_session_owns_mgr(&session, &mgr1, &foe, first));
    CHECK(ecat_upgrade_session_release_mgr(&session, &mgr1, &foe, first) == ECAT_UPGRADE_SESSION_STALE);
    CHECK(ecat_upgrade_session_owns_mgr(&session, &mgr2, &file, second));
    CHECK(ecat_upgrade_session_release_mgr(&session, &mgr2, &file, second) == ECAT_UPGRADE_SESSION_OK);
    session.generation = UINT64_MAX;
    CHECK(ecat_upgrade_session_acquire_mgr(&session, &mgr1, &foe, &rejected) == ECAT_UPGRADE_SESSION_EXHAUSTED);
    CHECK(session.owner == NULL && session.manager == NULL && rejected == 99);
    CHECK(!ecat_upgrade_session_owns_mgr(NULL, &mgr1, &foe, 1U));
    CHECK(!ecat_upgrade_session_owns_mgr(&session, &mgr1, NULL, 0U));
    puts("PASS: upgrade session ownership, stale tokens, contention and exhaustion");
    return 0;
}
