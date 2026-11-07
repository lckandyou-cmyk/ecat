#include "ecat_foe_ssc_async.h"

#include <string.h>

static void clear_packet(ecat_foe_ssc_async_t *a)
{
    a->pending = false;
    a->completed = false;
    a->following = false;
    a->operation_failed = false;
    a->packet_length = 0U;
    a->deadline = 0U;
}

static bool packet_matches(const ecat_foe_ssc_async_t *a,
                           const uint8_t *data,
                           size_t length,
                           bool following)
{
    if (a->packet_length != length || a->following != following) {
        return false;
    }
    if (length == 0U) {
        return true;
    }
    return data != NULL && memcmp(a->packet_storage, data, length) == 0;
}

static uint16_t finish_retry(ecat_foe_ssc_async_t *a)
{
    uint16_t result = a->operation_failed ? a->operation_error_foe_code : 0U;
    clear_packet(a);
    return result;
}

static bool deadline_expired(const ecat_foe_ssc_async_t *a, uint32_t now_ticks)
{
    return a->timeout_ticks != 0U &&
           (uint32_t)(now_ticks - a->deadline) < 0x80000000UL;
}

static void mark_timeout(ecat_foe_ssc_async_t *a)
{
    a->ops->abort(a->ops_ctx);
    a->pending = false;
    a->completed = true;
    a->operation_failed = true;
}

int ecat_foe_ssc_async_init(ecat_foe_ssc_async_t *a,
                            const ecat_foe_ssc_async_ops_t *ops,
                            void *ops_ctx,
                            uint8_t *packet_storage,
                            size_t packet_capacity,
                            uint32_t timeout_ticks,
                            uint16_t busy_foe_code,
                            uint16_t packet_error_foe_code,
                            uint16_t operation_error_foe_code)
{
    if (a == NULL || ops == NULL || ops->start == NULL || ops->poll == NULL ||
        ops->abort == NULL || packet_storage == NULL || packet_capacity == 0U ||
        timeout_ticks >= 0x80000000UL || busy_foe_code == 0U ||
        packet_error_foe_code == 0U || operation_error_foe_code == 0U) {
        return -1;
    }

    memset(a, 0, sizeof(*a));
    a->ops = ops;
    a->ops_ctx = ops_ctx;
    a->packet_storage = packet_storage;
    a->packet_capacity = packet_capacity;
    a->timeout_ticks = timeout_ticks;
    a->busy_foe_code = busy_foe_code;
    a->packet_error_foe_code = packet_error_foe_code;
    a->operation_error_foe_code = operation_error_foe_code;
    return 0;
}

uint16_t ecat_foe_ssc_async_on_data(ecat_foe_ssc_async_t *a,
                                    const uint8_t *data,
                                    size_t length,
                                    bool data_following,
                                    uint32_t now_ticks)
{
    if (a == NULL || a->ops == NULL || (length > 0U && data == NULL)) {
        return a != NULL ? a->packet_error_foe_code : 0x8004U;
    }

    if (a->pending || a->completed) {
        if (a->pending && deadline_expired(a, now_ticks)) {
            mark_timeout(a);
        }
        if (!packet_matches(a, data, length, data_following)) {
            return a->packet_error_foe_code;
        }
        if (a->pending) {
            return a->busy_foe_code;
        }
        return finish_retry(a);
    }

    if (length > a->packet_capacity) {
        return a->packet_error_foe_code;
    }

    if (length > 0U) {
        memcpy(a->packet_storage, data, length);
    }
    a->packet_length = length;
    a->following = data_following;
    a->deadline = now_ticks + a->timeout_ticks;

    ecat_foe_ssc_async_result_t result =
        a->ops->start(a->ops_ctx, a->packet_storage, length, data_following);
    if (result == ECAT_FOE_SSC_ASYNC_PENDING) {
        a->pending = true;
        return a->busy_foe_code;
    }
    if (result == ECAT_FOE_SSC_ASYNC_COMPLETE) {
        clear_packet(a);
        return 0U;
    }

    a->ops->abort(a->ops_ctx);
    clear_packet(a);
    return a->operation_error_foe_code;
}

uint16_t ecat_foe_ssc_async_poll(ecat_foe_ssc_async_t *a,
                                 uint32_t now_ticks)
{
    if (a == NULL || a->ops == NULL || !a->pending) {
        return a != NULL ? a->packet_error_foe_code : 0x8004U;
    }

    if (deadline_expired(a, now_ticks)) {
        mark_timeout(a);
        return a->operation_error_foe_code;
    }

    ecat_foe_ssc_async_result_t result = a->ops->poll(a->ops_ctx);
    if (result == ECAT_FOE_SSC_ASYNC_PENDING) {
        return a->busy_foe_code;
    }

    a->pending = false;
    a->completed = true;
    a->operation_failed = (result != ECAT_FOE_SSC_ASYNC_COMPLETE);
    if (a->operation_failed) {
        a->ops->abort(a->ops_ctx);
        return a->operation_error_foe_code;
    }
    return 0U;
}

void ecat_foe_ssc_async_abort(ecat_foe_ssc_async_t *a)
{
    if (a == NULL || a->ops == NULL) {
        return;
    }
    if (a->pending) {
        a->ops->abort(a->ops_ctx);
    }
    clear_packet(a);
}
