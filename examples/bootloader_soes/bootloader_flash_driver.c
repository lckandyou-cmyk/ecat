/**
 * @file bootloader_flash_driver.c
 * @brief 最小 Bootloader 目标存储 Flash 驱动参考实现
 */

#include "bootloader_flash_driver.h"
#include <string.h>

void bootloader_flash_driver_init_ctx(bootloader_flash_driver_t *drv,
                                      uint8_t *storage_buf,
                                      uint32_t storage_buf_size)
{
    if (drv == NULL) {
        return;
    }
    memset(drv, 0, sizeof(*drv));
    drv->base_address = APP_FLASH_BASE_ADDR;
    drv->max_size = APP_FLASH_MAX_SIZE;
    drv->sector_size = APP_FLASH_SECTOR_SIZE;
    drv->write_unit = APP_FLASH_WRITE_UNIT;

    drv->erased_watermark = 0;
    drv->written_bytes = 0;
    drv->valid_flag = APP_FLASH_INVALID_MAGIC;

    drv->is_initialized = false;
    drv->is_invalidated = false;
    drv->is_finalized = false;

    drv->storage_buf = storage_buf;
    drv->storage_buf_size = storage_buf_size;

    drv->inject_init_err = false;
    drv->inject_invalidate_err = false;
    drv->inject_erase_err = false;
    drv->inject_write_err = false;
    drv->inject_read_err = false;
    drv->inject_finalize_err = false;
}

/* 1. 介质初始化 */
static int drv_cb_init(void *user_ctx)
{
    bootloader_flash_driver_t *drv = (bootloader_flash_driver_t *)user_ctx;
    if (drv == NULL || drv->inject_init_err) {
        return -1;
    }
    drv->is_initialized = true;
    drv->erased_watermark = 0;
    drv->written_bytes = 0;
    return 0;
}

/* 2. 置无效标记 (擦除前必须持久化调用) */
static int drv_cb_invalidate(void *user_ctx)
{
    bootloader_flash_driver_t *drv = (bootloader_flash_driver_t *)user_ctx;
    if (drv == NULL || !drv->is_initialized || drv->inject_invalidate_err) {
        return -1;
    }
    /* 在擦除任何有效固件前，先将有效标记持久化置为非法 */
    drv->is_invalidated = true;
    drv->valid_flag = APP_FLASH_INVALID_MAGIC;
    return 0;
}

/* 3. 按需擦除扇区 */
static int drv_cb_erase_to(uint32_t need_end, uint32_t *erased_end, void *user_ctx)
{
    bootloader_flash_driver_t *drv = (bootloader_flash_driver_t *)user_ctx;
    if (drv == NULL || erased_end == NULL || !drv->is_initialized || !drv->is_invalidated) {
        return -1;
    }
    if (drv->inject_erase_err) {
        return -1;
    }
    if (need_end > drv->max_size) {
        return -1;
    }

    /* 向上对齐到物理扇区边界 */
    uint32_t aligned_end = ((need_end + drv->sector_size - 1U) / drv->sector_size) * drv->sector_size;
    if (aligned_end > drv->max_size) {
        aligned_end = drv->max_size;
    }

    if (aligned_end > drv->erased_watermark) {
        if (drv->storage_buf != NULL && aligned_end <= drv->storage_buf_size) {
            uint32_t erase_len = aligned_end - drv->erased_watermark;
            memset(&drv->storage_buf[drv->erased_watermark], 0xFF, erase_len);
        }
        drv->erased_watermark = aligned_end;
    }

    *erased_end = drv->erased_watermark;
    return 0;
}

/* 4. 分块写入 (物理基地址硬编码由本地配置独家裁决) */
static int drv_cb_write(uint32_t offset, const uint8_t *data, uint32_t len, void *user_ctx)
{
    bootloader_flash_driver_t *drv = (bootloader_flash_driver_t *)user_ctx;
    if (drv == NULL || data == NULL || !drv->is_initialized || !drv->is_invalidated) {
        return -1;
    }
    if (drv->inject_write_err) {
        return -1;
    }
    /* 对齐检查 */
    if (drv->write_unit > 0 && (len % drv->write_unit) != 0) {
        return -1;
    }
    /* 容量与擦除水位检查 */
    if (offset + len > drv->max_size || offset + len > drv->erased_watermark) {
        return -1;
    }

    /*
     * 关键安全断言：目标物理地址由本地配置宏 APP_FLASH_BASE_ADDR + offset 决定，
     * 任何外部数据包中的地址一律被忽略，防止越界踩踏 Bootloader 自身。
     */
    uint32_t physical_dest_addr = drv->base_address + offset;
    (void)physical_dest_addr; /* 实机驱动写入该物理地址 */

    if (drv->storage_buf != NULL && (offset + len) <= drv->storage_buf_size) {
        memcpy(&drv->storage_buf[offset], data, len);
    }
    if (offset + len > drv->written_bytes) {
        drv->written_bytes = offset + len;
    }
    return 0;
}

/* 5. 介质回读 (用于与数据流 CRC 双重比对) */
static int drv_cb_read(uint32_t offset, uint8_t *buf, uint32_t len, void *user_ctx)
{
    bootloader_flash_driver_t *drv = (bootloader_flash_driver_t *)user_ctx;
    if (drv == NULL || buf == NULL || !drv->is_initialized) {
        return -1;
    }
    if (drv->inject_read_err) {
        return -1;
    }
    if (offset + len > drv->max_size) {
        return -1;
    }

    if (drv->storage_buf != NULL && (offset + len) <= drv->storage_buf_size) {
        memcpy(buf, &drv->storage_buf[offset], len);
    } else {
        memset(buf, 0xFF, len);
    }
    return 0;
}

/* 6. 成功提交 (所有校验通过后写入有效标志) */
static int drv_cb_finalize(void *user_ctx)
{
    bootloader_flash_driver_t *drv = (bootloader_flash_driver_t *)user_ctx;
    if (drv == NULL || !drv->is_initialized || !drv->is_invalidated) {
        return -1;
    }
    if (drv->inject_finalize_err) {
        return -1;
    }
    drv->is_finalized = true;
    drv->valid_flag = APP_FLASH_VALID_MAGIC;
    return 0;
}

/* 7. 中止清理 (幂等操作) */
static void drv_cb_abort(void *user_ctx)
{
    bootloader_flash_driver_t *drv = (bootloader_flash_driver_t *)user_ctx;
    if (drv == NULL) {
        return;
    }
    drv->is_initialized = false;
    drv->is_finalized = false;
    /* 保持 valid_flag 为无效，防止 Bootloader 误启动 */
    drv->valid_flag = APP_FLASH_INVALID_MAGIC;
}

static const ecat_foe_target_driver_t s_bootloader_flash_ops = {
    .init       = drv_cb_init,
    .invalidate = drv_cb_invalidate,
    .erase_to   = drv_cb_erase_to,
    .write      = drv_cb_write,
    .read       = drv_cb_read,
    .finalize   = drv_cb_finalize,
    .abort      = drv_cb_abort
};

const ecat_foe_target_driver_t *bootloader_flash_driver_get_ops(void)
{
    return &s_bootloader_flash_ops;
}
