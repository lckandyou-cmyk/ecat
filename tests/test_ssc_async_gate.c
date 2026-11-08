#include "ecat_foe_ssc_async.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

typedef struct {
    unsigned starts;
    unsigned polls;
    unsigned aborts;
    unsigned pending_polls;
    bool fail_poll;
    bool last_following;
    size_t last_length;
    uint8_t observed[32];
} mock_t;

static ecat_foe_ssc_async_result_t mock_start(void *ctx, const uint8_t *data,
                                               size_t length, bool following)
{
    mock_t *m = (mock_t *)ctx;
    m->starts++;
    m->last_length = length;
    m->last_following = following;
    if (length > sizeof(m->observed)) return ECAT_FOE_SSC_ASYNC_FAILED;
    if (length > 0U) memcpy(m->observed, data, length);
    return ECAT_FOE_SSC_ASYNC_PENDING;
}

static ecat_foe_ssc_async_result_t mock_poll(void *ctx)
{
    mock_t *m = (mock_t *)ctx;
    m->polls++;
    if (m->polls <= m->pending_polls) return ECAT_FOE_SSC_ASYNC_PENDING;
    return m->fail_poll ? ECAT_FOE_SSC_ASYNC_FAILED : ECAT_FOE_SSC_ASYNC_COMPLETE;
}

static void mock_abort(void *ctx)
{
    ((mock_t *)ctx)->aborts++;
}

int main(void)
{
    static const ecat_foe_ssc_async_ops_t ops = {mock_start, mock_poll, mock_abort};
    ecat_foe_ssc_async_t adapter;
    mock_t mock = {0};
    uint8_t packet_storage[32];
    uint8_t packet[] = {1U, 2U, 3U, 4U};

    CHECK(ecat_foe_ssc_async_init(&adapter, &ops, &mock, packet_storage,
                                  sizeof(packet_storage), 10U,
                                  0x800AU, 0x800AU, 0x800BU) == -1);
    CHECK(ecat_foe_ssc_async_init(&adapter, &ops, &mock, packet_storage,
                                  sizeof(packet_storage), 10U,
                                  0x800AU, 0x8005U, 0x800AU) == -1);
    mock.pending_polls = 1U;
    CHECK(ecat_foe_ssc_async_init(&adapter, &ops, &mock, packet_storage,
                                  sizeof(packet_storage), 10U,
                                  0x800AU, 0x8005U, 0x800BU) == 0);
    CHECK(ecat_foe_ssc_async_on_data(&adapter, packet, sizeof(packet), true, 100U) == 0x800AU);
    CHECK(mock.starts == 1U && adapter.pending);
    memset(packet, 0xEE, sizeof(packet)); /* adapter must own a stable copy */
    CHECK(memcmp(mock.observed, (uint8_t[]){1U, 2U, 3U, 4U}, 4U) == 0);
    CHECK(ecat_foe_ssc_async_on_data(&adapter, mock.observed, 4U, true, 101U) == 0x800AU);
    CHECK(mock.starts == 1U); /* retry must not restart or enqueue twice */
    CHECK(ecat_foe_ssc_async_on_data(&adapter, (uint8_t[]){9U, 2U, 3U, 4U}, 4U, true, 101U) == 0x8005U);
    CHECK(adapter.pending && mock.starts == 1U); /* unrelated packet cannot replace pending data */
    CHECK(ecat_foe_ssc_async_poll(&adapter, 102U) == 0x800AU);
    CHECK(ecat_foe_ssc_async_poll(&adapter, 103U) == 0U);
    CHECK(adapter.completed && !adapter.pending);
    CHECK(ecat_foe_ssc_async_on_data(&adapter, mock.observed, 4U, true, 104U) == 0U);
    CHECK(!adapter.pending && !adapter.completed && mock.starts == 1U);

    /* Final/zero-length packet is identified by bDataFollowing, not packet size. */
    mock.polls = 0U;
    mock.pending_polls = 0U;
    CHECK(ecat_foe_ssc_async_on_data(&adapter, NULL, 0U, false, 200U) == 0x800AU);
    CHECK(mock.starts == 2U && !mock.last_following && mock.last_length == 0U);
    CHECK(ecat_foe_ssc_async_poll(&adapter, 201U) == 0U);
    CHECK(ecat_foe_ssc_async_on_data(&adapter, NULL, 0U, false, 202U) == 0U);

    /* Timeout arithmetic is wrap-safe and abort is exactly once. */
    mock.polls = 0U;
    mock.pending_polls = 100U;
    CHECK(ecat_foe_ssc_async_on_data(&adapter, (uint8_t[]){7U}, 1U, false, 0xFFFFFFFCU) == 0x800AU);
    CHECK(ecat_foe_ssc_async_poll(&adapter, 6U) == 0x800BU);
    CHECK(mock.aborts == 1U);
    CHECK(ecat_foe_ssc_async_on_data(&adapter, (uint8_t[]){7U}, 1U, false, 7U) == 0x800BU);
    CHECK(mock.aborts == 1U && !adapter.pending && !adapter.completed);

    /* Backend failure aborts once; retry returns error without rerunning work. */
    mock.polls = 0U;
    mock.pending_polls = 0U;
    mock.fail_poll = true;
    CHECK(ecat_foe_ssc_async_on_data(&adapter, (uint8_t[]){8U}, 1U, true, 300U) == 0x800AU);
    CHECK(ecat_foe_ssc_async_poll(&adapter, 301U) == 0x800BU);
    CHECK(mock.aborts == 2U);
    CHECK(ecat_foe_ssc_async_on_data(&adapter, (uint8_t[]){8U}, 1U, true, 302U) == 0x800BU);
    CHECK(mock.starts == 4U && mock.aborts == 2U);

    /* A late retransmission must observe timeout even before the next poll. */
    mock.polls = 0U;
    mock.pending_polls = 100U;
    mock.fail_poll = false;
    CHECK(ecat_foe_ssc_async_on_data(&adapter, (uint8_t[]){5U}, 1U, true, 400U) == 0x800AU);
    CHECK(ecat_foe_ssc_async_on_data(&adapter, (uint8_t[]){5U}, 1U, true, 410U) == 0x800BU);
    CHECK(mock.starts == 5U && mock.aborts == 3U);

    puts("PASS: SSC async gate BUSY/retry, packet identity, following flag, timeout and abort");
    return 0;
}
