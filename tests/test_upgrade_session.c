#include "ecat_upgrade_session.h"
#include <stdint.h>
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    ecat_upgrade_session_t session = {0};
    int foe = 0, file = 0;
    uint64_t first = 0, second = 0, rejected = 99;
    CHECK(ecat_upgrade_session_acquire(NULL, &foe, &first) == ECAT_UPGRADE_SESSION_INVALID);
    CHECK(ecat_upgrade_session_acquire(&session, NULL, &first) == ECAT_UPGRADE_SESSION_INVALID);
    CHECK(ecat_upgrade_session_acquire(&session, &foe, NULL) == ECAT_UPGRADE_SESSION_INVALID);
    CHECK(ecat_upgrade_session_acquire(&session, &foe, &first) == ECAT_UPGRADE_SESSION_OK);
    CHECK(first != 0 && ecat_upgrade_session_owns(&session, &foe, first));
    CHECK(ecat_upgrade_session_acquire(&session, &file, &rejected) == ECAT_UPGRADE_SESSION_BUSY);
    CHECK(rejected == 99 && ecat_upgrade_session_owns(&session, &foe, first));
    CHECK(ecat_upgrade_session_release(&session, &file, first) == ECAT_UPGRADE_SESSION_STALE);
    CHECK(ecat_upgrade_session_release(&session, &foe, first + 1) == ECAT_UPGRADE_SESSION_STALE);
    CHECK(ecat_upgrade_session_release(&session, &foe, first) == ECAT_UPGRADE_SESSION_OK);
    CHECK(ecat_upgrade_session_release(&session, &foe, first) == ECAT_UPGRADE_SESSION_STALE);
    CHECK(ecat_upgrade_session_acquire(&session, &file, &second) == ECAT_UPGRADE_SESSION_OK);
    CHECK(second > first && !ecat_upgrade_session_owns(&session, &foe, first));
    CHECK(ecat_upgrade_session_release(&session, &foe, first) == ECAT_UPGRADE_SESSION_STALE);
    CHECK(ecat_upgrade_session_owns(&session, &file, second));
    CHECK(ecat_upgrade_session_release(&session, &file, second) == ECAT_UPGRADE_SESSION_OK);
    session.generation = UINT64_MAX;
    CHECK(ecat_upgrade_session_acquire(&session, &foe, &rejected) == ECAT_UPGRADE_SESSION_EXHAUSTED);
    CHECK(session.owner == NULL && rejected == 99);
    CHECK(!ecat_upgrade_session_owns(NULL, &foe, 1));
    CHECK(!ecat_upgrade_session_owns(&session, NULL, 0));
    puts("PASS: upgrade session ownership, stale tokens, contention and exhaustion");
    return 0;
}
