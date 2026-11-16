/** Internal firmware policy operations. Applications use ecat_transfer.h. */
#ifndef ECAT_FOE_CORE_INTERNAL_H
#define ECAT_FOE_CORE_INTERNAL_H
#include "ecat_foe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Begin from transport-independent identity. Requires an IDLE configured core;
 * rejects active/terminal sessions without modifying them. Call on_abort before
 * reusing a terminal core. Returns a local core error, never a FoE wire code. */
ecat_foe_core_err_t ecat_foe_core_begin_request(
    ecat_foe_core_t *core, const ecat_upgrade_request_t *request);

/**
 * @brief 只读预检 FoE WRQ 写请求文件名及目标合法性 (不改变核心机状态或调用驱动)
 *
 * @param core           核心机上下文只读指针
 * @param filename       主站请求写入的文件名缓冲区
 * @param filename_len   文件名长度 (字节)
 * @param out_target_id  输出解析出的目标 ID (可选，传 NULL 忽略)
 * @param out_fw_version 输出解析出的固件版本号 (可选，传 NULL 忽略)
 * @return 0 校验通过合法；非 0 返回标准 FoE 错误码 (0x8001 / 0x8004 等)
 */
int ecat_foe_core_validate_wrq(const ecat_foe_core_t *core,
                               const char *filename,
                               size_t filename_len,
                               uint16_t *out_target_id,
                               uint32_t *out_fw_version);

/**
 * @brief 处理 FoE 数据块写入
 * 包含头部流式装配、擦除前全量核验、按需覆盖擦除、write_unit 残留缓冲对齐写入与即时多收熔断。
 * @param core 核心机上下文指针
 * @param data 接收到的数据块指针
 * @param len  数据块长度 (字节，len == 0 视为传输结束)
 * @return 0 成功；非 0 失败 (SOES 适配层收敛回复 0x800B)
 */
int ecat_foe_core_on_data(ecat_foe_core_t *core, const uint8_t *data, size_t len);

/**
 * @brief 处理传输结束通知
 * 补齐残留 0xFF 写入、比对流 CRC、物理介质分块回读 CRC 比对并调用 finalize()/abort()。
 * @param core 核心机上下文指针
 * @return 0 成功；非 0 失败
 */
int ecat_foe_core_on_end(ecat_foe_core_t *core);

/**
 * @brief 异常中止通知 (主站取消或网络异常)
 * 释放底层驱动并重置核心状态机。
 */
void ecat_foe_core_on_abort(ecat_foe_core_t *core);

#ifdef __cplusplus
}
#endif
#endif
