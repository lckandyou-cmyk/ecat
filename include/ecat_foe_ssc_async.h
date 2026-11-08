/**
 * @file ecat_foe_ssc_async.h
 * @brief Single-flight SSC FoE DATA/BUSY retry gate.
 *
 * This small, allocation-free adapter primitive owns one DATA packet while a
 * caller-driven asynchronous operation is pending. It does not implement the
 * ECAT core or a storage driver; applications must connect the callbacks to a
 * stateful asynchronous core before using it for firmware updates.
 */
#ifndef ECAT_FOE_SSC_ASYNC_H
#define ECAT_FOE_SSC_ASYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ECAT_FOE_SSC_ASYNC_FAILED = -1,
    ECAT_FOE_SSC_ASYNC_COMPLETE = 0,
    ECAT_FOE_SSC_ASYNC_PENDING = 1
} ecat_foe_ssc_async_result_t;

typedef struct {
    ecat_foe_ssc_async_result_t (*start)(void *ctx, const uint8_t *data,
                                         size_t length, bool data_following);
    ecat_foe_ssc_async_result_t (*poll)(void *ctx);
    void (*abort)(void *ctx);
} ecat_foe_ssc_async_ops_t;

typedef struct {
    const ecat_foe_ssc_async_ops_t *ops;
    void *ops_ctx;
    uint8_t *packet_storage;       /* Caller-owned static storage. */
    size_t packet_capacity;
    uint32_t timeout_ticks;
    uint32_t deadline;
    uint16_t busy_foe_code;
    uint16_t packet_error_foe_code;
    uint16_t operation_error_foe_code;
    bool pending;
    bool completed;
    bool following;
    bool operation_failed;
    size_t packet_length;
} ecat_foe_ssc_async_t;

/*
 * Initialize with caller-owned packet storage and SSC-specific FoE return
 * values. BUSY must differ from both error values. `ops`, storage, and
 * context must outlive the adapter.
 */
int ecat_foe_ssc_async_init(ecat_foe_ssc_async_t *adapter,
                            const ecat_foe_ssc_async_ops_t *ops,
                            void *ops_ctx,
                            uint8_t *packet_storage,
                            size_t packet_capacity,
                            uint32_t timeout_ticks,
                            uint16_t busy_foe_code,
                            uint16_t packet_error_foe_code,
                            uint16_t operation_error_foe_code);

/* Called by the SSC DATA callback. It never waits for a pending operation. */
uint16_t ecat_foe_ssc_async_on_data(ecat_foe_ssc_async_t *adapter,
                                    const uint8_t *data,
                                    size_t length,
                                    bool data_following,
                                    uint32_t now_ticks);

/* Call regularly from the application main loop; wrap-safe for uint32 ticks. */
uint16_t ecat_foe_ssc_async_poll(ecat_foe_ssc_async_t *adapter,
                                 uint32_t now_ticks);

/* Cancel a pending operation and release its packet state. */
void ecat_foe_ssc_async_abort(ecat_foe_ssc_async_t *adapter);

#ifdef __cplusplus
}
#endif

#endif
