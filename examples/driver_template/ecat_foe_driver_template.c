/**
 * @file ecat_foe_driver_template.c
 * @brief 目标驱动实现骨架模板
 *
 * 遵循严格生命周期契约：
 * 1. init 失败自行清理；
 * 2. init 成功后任何失败核心恰好调一次 abort；
 * 3. abort 幂等；
 * 4. invalidate 必须持久且早于擦除；
 * 5. finalize 最后写有效标记；
 * 6. Bootloader 启动需读取该标记。
 */

#include "ecat_foe_driver_template.h"

/* 1. 初始化回调 */
static int template_drv_init(void *user_ctx)
{
    ecat_foe_driver_template_ctx_t *ctx = (ecat_foe_driver_template_ctx_t *)user_ctx;
    if (ctx == (void *)0) {
        return -1;
    }

    /*
     * 硬件驱动操作：
     * 1. 初始化 SPI/QSPI 外设或内部 Flash 控制器；
     * 2. 读取芯片 JEDEC ID 校验通信与供电是否正常；
     * 3. 解锁相关 Flash 扇区写保护。
     *
     * 契约注意：若在此阶段发生任何失败，驱动必须在此自行执行必要的释放或重新加锁，
     * 然后返回非 0。核心状态机在 init 返回非 0 时不会调用 abort()。
     */
    ctx->is_initialized = true;
    ctx->is_invalidated = false;
    ctx->erased_watermark = 0;
    return 0;
}

/* 2. 置无效标记回调 (擦除前调用) */
static int template_drv_invalidate(void *user_ctx)
{
    ecat_foe_driver_template_ctx_t *ctx = (ecat_foe_driver_template_ctx_t *)user_ctx;
    if (ctx == (void *)0 || !ctx->is_initialized) {
        return -1;
    }

    /*
     * 硬件驱动操作：
     * 在擦除任何有效固件扇区之前，必须先将存储在元数据区或固定偏移处的
     * "固件有效" 标记清除或置为非法 (例如将 0x5A5AA5A5 覆写为 0x00000000)。
     * 该操作必须同步落盘，防止擦除过程中突发掉电导致 Bootloader 误启动半擦除固件。
     */
    ctx->is_valid = false;
    ctx->is_invalidated = true;
    return 0;
}

/* 3. 按需擦除回调 */
static int template_drv_erase_to(uint32_t need_end, uint32_t *erased_end, void *user_ctx)
{
    ecat_foe_driver_template_ctx_t *ctx = (ecat_foe_driver_template_ctx_t *)user_ctx;
    if (ctx == (void *)0 || erased_end == (void *)0 || !ctx->is_initialized || !ctx->is_invalidated) {
        return -1;
    }

    uint32_t sector_size = ctx->sector_size;
    if (sector_size == 0) {
        sector_size = 4096U;
    }

    /* 按扇区对齐向上擦除，直至覆盖 need_end */
    uint32_t current = ctx->erased_watermark;
    while (current < need_end) {
        if (current > ctx->max_size || sector_size > ctx->max_size - current) {
            /* 超过分区容量上限，擦除失败拒绝越界 */
            return -1;
        }

        /*
         * 硬件驱动操作：
         * 执行底层硬件 Flash 扇区擦除指令：
         * bsp_flash_erase_sector(ctx->base_address + current);
         */
        current += sector_size;
    }

    ctx->erased_watermark = current;
    *erased_end = current;
    return 0;
}

/* 4. 对齐分块写入回调 */
static int template_drv_write(uint32_t offset, const uint8_t *data, uint32_t len, void *user_ctx)
{
    ecat_foe_driver_template_ctx_t *ctx = (ecat_foe_driver_template_ctx_t *)user_ctx;
    if (ctx == (void *)0 || data == (void *)0 || !ctx->is_initialized) {
        return -1;
    }

    /*
     * 硬件驱动操作：
     * 调用硬件 Flash 页编程或双字编程接口：
     * bsp_flash_program(ctx->base_address + offset, data, len);
     */
    (void)offset;
    (void)len;
    return 0;
}

/* 5. 物理介质底层读取回调 (用于读回校验) */
static int template_drv_read(uint32_t offset, uint8_t *buf, uint32_t len, void *user_ctx)
{
    ecat_foe_driver_template_ctx_t *ctx = (ecat_foe_driver_template_ctx_t *)user_ctx;
    if (ctx == (void *)0 || buf == (void *)0 || !ctx->is_initialized) {
        return -1;
    }

    /*
     * 硬件驱动操作：
     * 从物理介质直接读取数据以供核心核验硬件回读 CRC32：
     * bsp_flash_read(ctx->base_address + offset, buf, len);
     */
    (void)offset;
    (void)len;
    return 0;
}

/* 6. 成功提交回调 (最后写有效标记) */
static int template_drv_finalize(void *user_ctx)
{
    ecat_foe_driver_template_ctx_t *ctx = (ecat_foe_driver_template_ctx_t *)user_ctx;
    if (ctx == (void *)0 || !ctx->is_initialized) {
        return -1;
    }

    /*
     * 硬件驱动操作：
     * 仅当所有数据写入并通过流 CRC 及物理介质回读 CRC32 双重校验后触发。
     * 在此将固件有效标志写入元数据区 (例如写入 0x5A5AA5A5)，
     * 并重新加锁 Flash 控制器写保护。
     * Bootloader 启动检测时将依赖此标记决定是否引导新固件。
     */
    ctx->is_valid = true;
    ctx->is_invalidated = false;
    return 0;
}

/* 7. 中止清理回调 (具备幂等性) */
static void template_drv_abort(void *user_ctx)
{
    ecat_foe_driver_template_ctx_t *ctx = (ecat_foe_driver_template_ctx_t *)user_ctx;
    if (ctx == (void *)0) {
        return;
    }

    /*
     * 硬件驱动操作：
     * 1. 放弃所有未完成操作，关闭写使能，重新开启写保护；
     * 2. 保持有效标记处于无效状态；
     * 3. 驱动需保证此函数可被安全调用多次 (幂等性)。
     */
    ctx->is_valid = false;
    ctx->is_invalidated = false;
    ctx->erased_watermark = 0;
    ctx->is_initialized = false;
}

static const ecat_foe_target_driver_t s_template_driver = {
    .init = template_drv_init,
    .invalidate = template_drv_invalidate,
    .erase_to = template_drv_erase_to,
    .write = template_drv_write,
    .read = template_drv_read,
    .finalize = template_drv_finalize,
    .abort = template_drv_abort,
    /* 防回滚默认关闭；启用前必须实现断电安全的联合提交事务。 */
    .finalize_with_version_floor = NULL
};

const ecat_foe_target_driver_t *ecat_foe_driver_template_get_ops(void)
{
    return &s_template_driver;
}
