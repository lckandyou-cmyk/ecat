/** Optional host FILE stream reference transport; not a hardware protocol. */
#ifndef ECAT_UPGRADE_FILE_H
#define ECAT_UPGRADE_FILE_H
#include "ecat_upgrade.h"
#include "ecat_transfer.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ecat_upgrade_t *upgrade;
    ecat_transfer_t *transfer;
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

/**
 * @brief 使用权威统一双向传输管理器初始化文件流适配器
 * @param file 文件传输句柄指针
 * @param transfer 权威统一双向传输管理器指针
 * @param buffer 调用方分配的数据缓冲区指针
 * @param capacity 缓冲区容量 (字节)
 * @return 0 成功，-1 参数非法
 */
int ecat_upgrade_file_init_transfer(ecat_upgrade_file_t *file, ecat_transfer_t *transfer,
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
