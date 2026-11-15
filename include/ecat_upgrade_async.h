/** Transport-independent single-operation asynchronous backend contract. */
#ifndef ECAT_UPGRADE_ASYNC_H
#define ECAT_UPGRADE_ASYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ECAT_UPGRADE_ASYNC_FAILED = -1,
    ECAT_UPGRADE_ASYNC_COMPLETE = 0,
    ECAT_UPGRADE_ASYNC_PENDING = 1
} ecat_upgrade_async_result_t;

/* start copies or owns data before returning; more_data=false requests end/commit.
 * Only one operation may be pending. poll advances it, never restarts it.
 * abort cancels backend work and MUST synchronously terminate all hardware/DMA
 * access to the staging buffer before returning. Drivers unable to stop hardware
 * synchronously must not use this contract without double buffering.
 * COMPLETE indicates all hardware I/O to the buffer has fully finished.
 * All callbacks and the owning session manager must run serially. */
typedef struct {
    ecat_upgrade_async_result_t (*start)(void *ctx, const uint8_t *data,
                                       size_t length, bool more_data);
    ecat_upgrade_async_result_t (*poll)(void *ctx);
    void (*abort)(void *ctx);
} ecat_upgrade_async_ops_t;

#ifdef __cplusplus
}
#endif
#endif
