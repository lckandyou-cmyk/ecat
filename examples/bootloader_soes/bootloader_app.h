/**
 * @file bootloader_app.h
 * @brief 最小 Bootloader 核心应用层模块头文件
 *
 * 负责串联本地配置、目标存储驱动、核心升级状态机与 SOES 适配层，
 * 提供确定性的前置/SOES处理/后置串行调用与异常恢复调度。
 */

#ifndef BOOTLOADER_APP_H
#define BOOTLOADER_APP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "ecat_foe_core.h"
#include "ecat_foe_soes.h"
#include "bootloader_config.h"
#include "bootloader_flash_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bootloader 应用层主上下文结构体
 */
typedef struct {
    ecat_foe_core_t           core;           /**< 核心升级状态机实例 */
    ecat_foe_soes_adapter_t   adapter;        /**< SOES 协议栈适配器实例 */
    bootloader_flash_driver_t flash_drv;      /**< 目标存储 Flash 驱动实例 */

    uint32_t                  last_al_error;  /**< 最近捕获的 AL 状态错误码 */
    bool                      is_initialized; /**< Bootloader 实例是否已就绪 */
} bootloader_app_t;

/**
 * @brief 初始化 Bootloader 应用层
 *
 * 完成板级配置载入、驱动注册、SOES 邮箱与文件表映射绑定，
 * 并注入 ALerror 及 FOE_init 状态机复位回调。
 *
 * @param app             Bootloader 上下文指针
 * @param foe_cfg         SOES 的 foe_cfg 配置结构体指针
 * @param mbxcontrol      SOES 的 _MBXcontrol 数组指针
 * @param mbx             SOES 的 MBX 数据缓冲区首地址
 * @param num_slots       SOES 邮箱槽位总数 (如 MBXBUFFERS)
 * @param slot_size       每个槽位容量 (如 MBXSIZE)
 * @param storage_buf     模拟 Flash 存储内存 (宿主测试使用)
 * @param storage_size    模拟 Flash 尺寸
 * @param reset_fn        协议栈状态机复位函数指针 (真实环境传入 FOE_init)
 * @param al_error_fn     AL 状态错误设置函数指针 (真实环境传入 ESC_ALerror)
 * @return 0 成功，非 0 失败
 */
int bootloader_app_init(bootloader_app_t *app,
                        foe_cfg_t *foe_cfg,
                        void *mbxcontrol,
                        uint8_t *mbx,
                        uint8_t num_slots,
                        uint16_t slot_size,
                        uint8_t *storage_buf,
                        uint32_t storage_size,
                        ecat_foe_reset_fn_t reset_fn,
                        ecat_foe_al_error_fn_t al_error_fn);

/**
 * @brief Bootloader 单步轮询调度函数 (pre -> ESC_foeprocess -> post 串行流)
 *
 * 核心调度时序：
 * 1. pre_process_hook: 只读嗅探接收邮箱，解析文件名，做只读预检并暂存 WRQ；
 * 2. foeprocess_fn: 真实 SOES 执行原生密码/BOOT 状态核验并排队响应 (若传入 NULL 则调用默认 ESC_foeprocess)；
 * 3. post_process_hook: 核验输出槽位，若核心激活失败或状态冲突，就地中和为 0x8000 ERR 并触发 FOE_init 复位。
 *
 * @param app             Bootloader 上下文指针
 * @param foe_cfg         SOES 的 foe_cfg 配置结构体指针
 * @param rx_buf          SOES 输入邮箱槽位 0 首地址；无输入时传 NULL
 * @param rx_len          已实际接收的报文总字节数；无输入时传 0
 * @param tx_buf          待发送或已排队的邮箱报文首地址 (可为 NULL 由后置钩子通过槽位自动识别)
 * @param tx_len          待发送报文长度
 * @param foeprocess_fn   SOES 协议机主处理函数指针 (可为 NULL 默认调用 ESC_foeprocess)
 * @return 0 正常流转；-1 失败。结构错误时不调用 SOES 或生成响应，
 *         邮箱所有者必须丢弃输入槽位，再依设备策略恢复。
 */
int bootloader_app_step(bootloader_app_t *app,
                        foe_cfg_t *foe_cfg,
                        const uint8_t *rx_buf,
                        size_t rx_len,
                        const uint8_t *tx_buf,
                        size_t tx_len,
                        void (*foeprocess_fn)(void));

/**
 * @brief 主动中止升级并复位核心及已配置的 SOES 会话
 *
 * 未提供 reset_fn 时，调用方须在重新传输前复位协议栈。
 *
 * @param app Bootloader 上下文指针
 */
void bootloader_app_abort(bootloader_app_t *app);

#ifdef __cplusplus
}
#endif

#endif /* BOOTLOADER_APP_H */
