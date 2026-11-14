/**
 * @file mock_driver.c
 * @brief RAM 模拟目标驱动实现
 */

#include "mock_driver.h"
#include <string.h>

static void record_op(mock_driver_t *m, mock_drv_op_t op)
{
    if (m->op_count < (sizeof(m->op_history) / sizeof(m->op_history[0]))) {
        m->op_history[m->op_count++] = op;
    }
}

static int mock_init(void *user_ctx)
{
    mock_driver_t *m = (mock_driver_t *)user_ctx;
    m->count_init++;
    record_op(m, MOCK_OP_INIT);
    if (m->inject_fail_init) {
        return -1;
    }
    m->erased_watermark = 0U;
    return 0;
}

static int mock_invalidate(void *user_ctx)
{
    mock_driver_t *m = (mock_driver_t *)user_ctx;
    m->count_invalidate++;
    record_op(m, MOCK_OP_INVALIDATE);
    if (m->inject_fail_invalidate) {
        return -1;
    }
    m->valid_flag = false;
    return 0;
}

static int mock_erase_to(uint32_t need_end, uint32_t *erased_end, void *user_ctx)
{
    mock_driver_t *m = (mock_driver_t *)user_ctx;
    m->count_erase++;
    record_op(m, MOCK_OP_ERASE_TO);
    if (m->inject_fail_erase) {
        return -1;
    }

    if (need_end > MOCK_DRIVER_MEM_SIZE) {
        return -1;
    }

    /* 按 4KB 扇区对齐擦除覆盖到 need_end */
    uint32_t sectors = (need_end + MOCK_DRIVER_SECTOR_SIZE - 1U) / MOCK_DRIVER_SECTOR_SIZE;
    uint32_t actual_end = sectors * MOCK_DRIVER_SECTOR_SIZE;
    if (actual_end > MOCK_DRIVER_MEM_SIZE) {
        actual_end = MOCK_DRIVER_MEM_SIZE;
    }

    /* 模拟 Flash 擦除: 全置 0xFF */
    if (actual_end > m->erased_watermark) {
        memset(m->mem + m->erased_watermark, 0xFF, actual_end - m->erased_watermark);
        m->erased_watermark = actual_end;
    }
    if (erased_end != NULL) {
        if (m->inject_insufficient_erase) {
            *erased_end = (need_end > 0) ? (need_end - 1U) : 0U;
        } else {
            *erased_end = actual_end;
        }
    }
    return 0;
}

static int mock_write(uint32_t offset, const uint8_t *data, uint32_t len, void *user_ctx)
{
    mock_driver_t *m = (mock_driver_t *)user_ctx;
    m->count_write++;
    record_op(m, MOCK_OP_WRITE);
    if (m->inject_fail_write) {
        return -1;
    }

    if (offset + len > MOCK_DRIVER_MEM_SIZE) {
        return -1;
    }

    /* 真实 Flash 物理写入语义: 只能把 1 变成 0，若未擦除导致 0 试图变 1 则报错 */
    for (uint32_t i = 0; i < len; i++) {
        uint8_t cur = m->mem[offset + i];
        uint8_t in = data[i];
        if ((cur & in) != in) {
            /* 存在 bit 从 0 变成 1，未擦除不可写 */
            return -2;
        }
        m->mem[offset + i] &= in;
    }
    return 0;
}

static int mock_read(uint32_t offset, uint8_t *buf, uint32_t len, void *user_ctx)
{
    mock_driver_t *m = (mock_driver_t *)user_ctx;
    m->count_read++;
    record_op(m, MOCK_OP_READ);
    if (m->inject_fail_read) {
        return -1;
    }

    if (offset + len > MOCK_DRIVER_MEM_SIZE) {
        return -1;
    }

    memcpy(buf, m->mem + offset, len);
    if (m->inject_corrupt_read && len > 0) {
        buf[0] ^= 0xFF; /* 翻转首字节破坏 CRC */
    }
    return 0;
}

static int mock_finalize(void *user_ctx)
{
    mock_driver_t *m = (mock_driver_t *)user_ctx;
    m->count_finalize++;
    record_op(m, MOCK_OP_FINALIZE);
    if (m->inject_fail_finalize) {
        return -1;
    }
    m->valid_flag = true;
    return 0;
}

static void mock_abort(void *user_ctx)
{
    mock_driver_t *m = (mock_driver_t *)user_ctx;
    m->count_abort++;
    record_op(m, MOCK_OP_ABORT);
    m->valid_flag = false;
}

void mock_driver_init(mock_driver_t *mock)
{
    if (mock == NULL) {
        return;
    }
    memset(mock, 0, sizeof(*mock));
    memset(mock->mem, 0xFF, sizeof(mock->mem)); /* 初始处于全擦除态 */
    mock->valid_flag = false;

    mock->driver.init = mock_init;
    mock->driver.invalidate = mock_invalidate;
    mock->driver.erase_to = mock_erase_to;
    mock->driver.write = mock_write;
    mock->driver.read = mock_read;
    mock->driver.finalize = mock_finalize;
    mock->driver.abort = mock_abort;
}

void mock_driver_reset(mock_driver_t *mock)
{
    if (mock == NULL) {
        return;
    }
    mock_driver_init(mock);
}
