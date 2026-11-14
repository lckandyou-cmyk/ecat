/**
 * @file ecat_foe_driver_template.h
 * @brief 目标驱动实现模板头文件 (包含生命周期契约说明)
 *
 * 本模板为用户将 EtherCAT FoE 核心状态机接入具体硬件介质 (如片内 Flash、
 * 外部 SPI NOR Flash、EEPROM 等) 提供参考骨架与契约规范。
 *
 * 核心与驱动间的生命周期契约：
 * 1. 【init 失败自行清理】：
 *    若 init() 返回非 0 失败，表示初始化未建立，驱动需在返回前自行清理半成品资源。
 *    核心状态机不会调用 abort()。
 * 2. 【init 成功后失败恰好调一次 abort】：
 *    一旦 init() 成功返回 0，驱动会话正式激活。其后无论是 invalidate 失败、擦除失败、
 *    写入失败、流 CRC 不匹配、回读校验不符还是外部中断，核心保证恰好调用一次 abort()。
 * 3. 【abort 幂等】：
 *    abort() 回调实现必须具备幂等性，无论驱动当前处于何种中间状态，重复或空闲调用均应安全返回。
 * 4. 【invalidate 必须持久且早于擦除】：
 *    在擦除任何扇区之前必须先将介质上的有效标志持久化置为无效。防止擦除或写入中途断电时，
 *    Bootloader 误将残缺固件当作有效固件执行。
 * 5. 【finalize 最后写有效标记】：
 *    普通模式的 finalize() 仅在接收完整、流 CRC 与回读 CRC 通过且启用时验签成功后调用；防回滚模式使用 finalize_with_version_floor() 联合提交。
 *    驱动在确认全部写入无误后，最后将有效标记写入非易失介质。
 * 6. 【Bootloader 启动契约】：
 *    Bootloader 启动时必须读取该有效标记。若标记缺失或处于无效状态，必须拒绝跳转并停留在 Bootloader。
 */

#ifndef ECAT_FOE_DRIVER_TEMPLATE_H
#define ECAT_FOE_DRIVER_TEMPLATE_H

#include <stdint.h>
#include <stdbool.h>
#include "ecat_foe_target.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 驱动模板自定义上下文示例
 */
typedef struct {
    uint32_t base_address;     /**< 分区在 Flash 中的物理基地址 */
    uint32_t sector_size;      /**< Flash 物理扇区大小 (例如 4096 字节) */
    uint32_t max_size;         /**< 分区最大允许容量 (字节) */
    uint32_t erased_watermark; /**< 当前已擦除的水位线 (字节偏移) */
    bool     is_initialized;   /**< 介质是否已初始化 */
    bool     is_invalidated;    /**< 本次会话已执行失效操作 */
    bool     is_valid;         /**< 介质有效标记状态 */
} ecat_foe_driver_template_ctx_t;

/**
 * @brief 获取驱动模板函数指针操作集
 * @return 指向静态驱动操作集的指针
 */
const ecat_foe_target_driver_t *ecat_foe_driver_template_get_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_FOE_DRIVER_TEMPLATE_H */
