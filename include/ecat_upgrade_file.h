/** Optional host FILE stream reference transport; not a hardware protocol. */
#ifndef ECAT_UPGRADE_FILE_H
#define ECAT_UPGRADE_FILE_H
#include "ecat_upgrade.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ecat_upgrade_t *upgrade;
    FILE *stream;
    uint8_t *buffer;
    size_t capacity;
    uint64_t token;
    bool active;
    bool pending;
    bool ending;
    bool io_error;
    ecat_upgrade_result_t result;
} ecat_upgrade_file_t;

/* Initialize once; do not copy. Caller owns stream and buffer lifetime. */
int ecat_upgrade_file_init(ecat_upgrade_file_t *file, ecat_upgrade_t *upgrade,
    uint8_t *buffer, size_t capacity);
/* Accept a binary stream positioned at its EFW header. Busy never reads it. */
ecat_upgrade_result_t ecat_upgrade_file_begin(ecat_upgrade_file_t *file,
    FILE *stream, const ecat_upgrade_request_t *request);
/* One read/write or poll per step. Returns PENDING until explicit EOF finish
 * commits. fread may block: this is a host reference, not an ISR/real-time API.
 * For async targets capacity must fit the configured engine packet buffer.
 * On I/O failure io_error=true and only this transport's lease is cancelled.
 * No function closes or rewinds the caller's stream. */
ecat_upgrade_result_t ecat_upgrade_file_step(ecat_upgrade_file_t *file);
ecat_upgrade_result_t ecat_upgrade_file_cancel(ecat_upgrade_file_t *file);

#ifdef __cplusplus
}
#endif
#endif
