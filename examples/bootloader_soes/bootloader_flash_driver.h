/**
 * @file bootloader_flash_driver.h
 * @brief 最小 Bootloader 目标存储 Flash 驱动接口定义
 *
 * 实现 ecat_foe_target_driver_t 规定的七大生命周期回调，
 * 遵循严格的本地地址裁决契约：写入物理基地址由本地配置决定，
 * 杜绝外部数据传入篡改基地址。
 */

#ifndef BOOTLOADER_FLASH_DRIVER_H
#define BOOTLOADER_FLASH_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ecat_foe_target.h"
#include "bootloader_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bootloader Flash 目标存储驱动运行上下文
 */
typedef struct {
    uint32_t base_address;          /**< 本地硬编码应用分区物理基地址 (APP_FLASH_BASE_ADDR) */
    uint32_t max_size;              /**< 本地硬编码应用分区容量上限 (APP_FLASH_MAX_SIZE) */
    uint32_t sector_size;           /**< 扇区大小 (APP_FLASH_SECTOR_SIZE) */
    uint32_t write_unit;            /**< 物理写入对齐单位 (APP_FLASH_WRITE_UNIT) */

    uint32_t erased_watermark;      /**< 当前物理已擦除的水位线偏移 */
    uint32_t written_bytes;         /**< 累计已写入字节数 */
    uint32_t valid_flag;            /**< 固件有效标志 (APP_FLASH_VALID_MAGIC / INVALID) */

    bool     is_initialized;        /**< 介质是否已初始化 (init 成功) */
    bool     is_invalidated;        /**< 介质有效标记是否已清除 (invalidate 成功) */
    bool     is_finalized;          /**< 升级是否已成功提交 (finalize 成功) */

    /* 宿主模拟介质缓冲区 (实机移植时对接真实 HAL 驱动寄存器) */
    uint8_t *storage_buf;           /**< 模拟物理 Flash 存储内存 */
    uint32_t storage_buf_size;      /**< 缓冲区尺寸 */

    /* 故障注入开关 (供宿主集成验证使用) */
    bool     inject_init_err;       /**< 注入 init 失败 */
    bool     inject_invalidate_err; /**< 注入 invalidate 失败 */
    bool     inject_erase_err;      /**< 注入 erase_to 失败 */
    bool     inject_write_err;      /**< 注入 write 失败 */
    bool     inject_read_err;       /**< 注入 read 失败 */
    bool     inject_finalize_err;   /**< 注入 finalize 失败 */
} bootloader_flash_driver_t;

/**
 * @brief 初始化 Flash 驱动上下文
 *
 * @param drv              驱动上下文指针
 * @param storage_buf      宿主模拟介质缓冲区指针
 * @param storage_buf_size 缓冲区尺寸
 */
void bootloader_flash_driver_init_ctx(bootloader_flash_driver_t *drv,
                                      uint8_t *storage_buf,
                                      uint32_t storage_buf_size);

/**
 * @brief 获取 Bootloader Flash 驱动函数操作集 (满足 ecat_foe_target_driver_t 契约)
 *
 * @return 指向静态驱动操作集的指针
 */
const ecat_foe_target_driver_t *bootloader_flash_driver_get_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOTLOADER_FLASH_DRIVER_H */
