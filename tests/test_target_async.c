#include "ecat_foe_target_async.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned starts;
    unsigned polls;
    unsigned aborts;
    unsigned pending_polls;
    ecat_foe_target_op_kind_t kind;
    uint8_t observed[16];
    uint32_t observed_len;
    uint16_t observed_target;
    uint32_t observed_version;
    uint32_t observed_floor;
    bool observed_anti_rollback;
} mock_t;

static ecat_foe_target_async_result_t mock_start(void *ctx,
                                                  ecat_foe_target_async_op_t *op)
{
    mock_t *m = (mock_t *)ctx;
    m->starts++;
    m->polls = 0U;
    m->kind = op->kind;
    m->observed_target = op->target_id;
    m->observed_version = op->fw_version;
    m->observed_floor = op->min_allow_version;
    m->observed_anti_rollback = op->anti_rollback;
    m->observed_len = op->length;
    if (op->kind == ECAT_FOE_TARGET_OP_WRITE && op->length <= sizeof(m->observed)) {
        memcpy(m->observed, op->data, op->length);
    }
    if (op->kind == ECAT_FOE_TARGET_OP_READ) {
        memset(op->data, 0xA5, op->length);
    }
    if (op->kind == ECAT_FOE_TARGET_OP_ERASE_TO) {
        op->erased_end = op->need_end + 32U;
    }
    return m->pending_polls == 0U ? ECAT_FOE_TARGET_ASYNC_COMPLETE
                                  : ECAT_FOE_TARGET_ASYNC_PENDING;
}

static ecat_foe_target_async_result_t mock_poll(void *ctx,
                                                 ecat_foe_target_async_op_t *op)
{
    mock_t *m = (mock_t *)ctx;
    m->polls++;
    if (m->polls < m->pending_polls) return ECAT_FOE_TARGET_ASYNC_PENDING;
    if (op->kind == ECAT_FOE_TARGET_OP_ERASE_TO) op->erased_end = op->need_end + 64U;
    if (op->kind == ECAT_FOE_TARGET_OP_READ) memset(op->data, 0x5A, op->length);
    return ECAT_FOE_TARGET_ASYNC_COMPLETE;
}

static void mock_abort(void *ctx)
{
    ((mock_t *)ctx)->aborts++;
}

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

int main(void)
{
    const ecat_foe_target_async_ops_t ops = { mock_start, mock_poll, mock_abort, NULL };
    ecat_foe_target_async_t runner;
    mock_t mock;
    uint8_t scratch[16];
    memset(&mock, 0, sizeof(mock));
    mock.pending_polls = 2U;
    CHECK(ecat_foe_target_async_init(&runner, &ops, &mock, scratch, sizeof(scratch)) == 0);

    uint8_t source[] = {1U, 2U, 3U, 4U};
    CHECK(ecat_foe_target_async_start(&runner, ECAT_FOE_TARGET_OP_WRITE,
          8U, 0U, source, sizeof(source), NULL, NULL) == ECAT_FOE_TARGET_ASYNC_PENDING);
    memset(source, 0xEE, sizeof(source)); /* staged copy must remain stable */
    CHECK(memcmp(mock.observed, (uint8_t[]){1U, 2U, 3U, 4U}, 4U) == 0);
    CHECK(ecat_foe_target_async_start(&runner, ECAT_FOE_TARGET_OP_INIT,
          0U, 0U, NULL, 0U, NULL, NULL) == ECAT_FOE_TARGET_ASYNC_FAILED);
    CHECK(ecat_foe_target_async_poll(&runner) == ECAT_FOE_TARGET_ASYNC_PENDING);
    CHECK(ecat_foe_target_async_poll(&runner) == ECAT_FOE_TARGET_ASYNC_COMPLETE);
    CHECK(!runner.pending && mock.starts == 1U && mock.polls == 2U);

    uint8_t readback[4] = {0U};
    uint32_t erased_end = 0U;
    CHECK(ecat_foe_target_async_start(&runner, ECAT_FOE_TARGET_OP_READ,
          0U, 0U, NULL, sizeof(readback), readback, NULL) == ECAT_FOE_TARGET_ASYNC_PENDING);
    CHECK(ecat_foe_target_async_poll(&runner) == ECAT_FOE_TARGET_ASYNC_PENDING);
    CHECK(ecat_foe_target_async_poll(&runner) == ECAT_FOE_TARGET_ASYNC_COMPLETE);
    CHECK(memcmp(readback, (uint8_t[]){0x5AU, 0x5AU, 0x5AU, 0x5AU}, 4U) == 0);

    CHECK(ecat_foe_target_async_start(&runner, ECAT_FOE_TARGET_OP_ERASE_TO,
          0U, 128U, NULL, 0U, NULL, &erased_end) == ECAT_FOE_TARGET_ASYNC_PENDING);
    CHECK(ecat_foe_target_async_poll(&runner) == ECAT_FOE_TARGET_ASYNC_PENDING);
    CHECK(ecat_foe_target_async_poll(&runner) == ECAT_FOE_TARGET_ASYNC_COMPLETE);
    CHECK(erased_end == 192U);

    CHECK(ecat_foe_target_async_start(&runner, ECAT_FOE_TARGET_OP_FINALIZE,
          0U, 0U, NULL, 0U, NULL, NULL) == ECAT_FOE_TARGET_ASYNC_PENDING);
    ecat_foe_target_async_abort(&runner);
    CHECK(!runner.pending && runner.failed && mock.aborts == 1U);
    ecat_foe_target_async_abort(&runner);
    CHECK(mock.aborts == 1U);
    CHECK(ecat_foe_target_async_start(&runner, ECAT_FOE_TARGET_OP_INIT,
          0U, 0U, NULL, 0U, NULL, NULL) == ECAT_FOE_TARGET_ASYNC_FAILED);
    CHECK(ecat_foe_target_async_reset(&runner) == 0);
    mock.pending_polls = 0U;
    CHECK(ecat_foe_target_async_start(&runner, ECAT_FOE_TARGET_OP_INIT,
          0U, 0U, NULL, 0U, NULL, NULL) == ECAT_FOE_TARGET_ASYNC_COMPLETE);
    CHECK(ecat_foe_target_async_start_finalize(&runner, 1U, 0x01020304U,
          0x01000000U, true) == ECAT_FOE_TARGET_ASYNC_COMPLETE);
    CHECK(mock.kind == ECAT_FOE_TARGET_OP_FINALIZE && mock.observed_target == 1U &&
          mock.observed_version == 0x01020304U && mock.observed_floor == 0x01000000U &&
          mock.observed_anti_rollback);

    puts("PASS: async target op runner stages WRITE, polls READ/ERASE and cancels once");
    return 0;
}
