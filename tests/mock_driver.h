/**
 * @file mock_driver.h
 * @brief RAM 模拟目标驱动，支持扇区擦除语义与故障注入
 */

#ifndef MOCK_DRIVER_H
#define MOCK_DRIVER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ecat_foe_target.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOCK_DRIVER_MEM_SIZE    (1024U * 1024U)  /* 1MB RAM 存储空间 */
#define MOCK_DRIVER_SECTOR_SIZE (4096U)          /* 4KB 扇区大小 */

typedef enum {
    MOCK_OP_INIT = 1,
    MOCK_OP_INVALIDATE,
    MOCK_OP_ERASE_TO,
    MOCK_OP_WRITE,
    MOCK_OP_READ,
    MOCK_OP_FINALIZE,
    MOCK_OP_ABORT
} mock_drv_op_t;

typedef struct {
    uint8_t  mem[MOCK_DRIVER_MEM_SIZE];          /**< 物理存储介质 RAM 模拟 */
    bool     valid_flag;                         /**< 固件有效标记 (invalidate 清除, finalize 置位) */

    /* 故障注入开关 */
    bool     inject_fail_init;
    bool     inject_fail_invalidate;
    bool     inject_fail_erase;
    bool     inject_fail_write;
    bool     inject_fail_read;
    bool     inject_fail_finalize;
    bool     inject_corrupt_read;                /**< 读取时翻转数据比特破坏 CRC */

    /* 调用计数器 */
    uint32_t count_init;
    uint32_t count_invalidate;
    uint32_t count_erase;
    uint32_t count_write;
    uint32_t count_read;
    uint32_t count_finalize;
    uint32_t count_abort;

    /* 调用顺序历史追踪 */
    mock_drv_op_t op_history[64];
    uint32_t      op_count;

    ecat_foe_target_driver_t driver;             /**< 驱动接口结构体 */
} mock_driver_t;

void mock_driver_init(mock_driver_t *mock);
void mock_driver_reset(mock_driver_t *mock);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_DRIVER_H */
