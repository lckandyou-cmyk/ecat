/**
 * @file ecat_foe_ssc.h
 * @brief Beckhoff Slave Stack Code (SSC) FoE 适配转接层
 *
 * 为 Beckhoff SSC (如 foeappl.c / bootmode.c) 提供轻量、无侵入的转接接口。
 * 用户无需修改 SSC 内部状态机，直接将 SSC 的 FoE 回调重定向到本模块即可。
 */

#ifndef ECAT_FOE_SSC_H
#define ECAT_FOE_SSC_H

#include "ecat_foe.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 转接 SSC 的 FOE_Write (或 BL_FoeWrite) 回调
 *
 * @param p_name 文件名指针 (来自 SSC 邮箱缓冲区)
 * @param name_len 文件名长度 (字节)
 * @param password 访问密码
 * @return 0 成功 (ACK)，非 0 返回 FoE 错误码 (如 0x8002, 0x8004 等)
 */
uint16_t ecat_foe_ssc_write(const void *p_name, uint16_t name_len, uint32_t password);

/**
 * @brief 转接 SSC 的 FOE_Data (或 BL_FoeData) 回调
 *
 * @param p_data 数据缓冲区指针
 * @param size 本包数据字节数
 * @param b_last 是否为最后一包数据
 * @return 0 成功 (ACK)，非 0 返回 FoE 错误码
 */
uint16_t ecat_foe_ssc_data(const void *p_data, uint16_t size, bool b_last);

/**
 * @brief 转接 SSC 的 FOE_Read (或 BL_FoeRead) 回调
 */
uint16_t ecat_foe_ssc_read(const void *p_name, uint16_t name_len, uint32_t password,
                          uint16_t max_block_size, void *p_data);

/**
 * @brief 转接 SSC 的 FOE_Error (或 BL_FoeError) 回调 (主站中止时调用)
 */
void ecat_foe_ssc_error(uint32_t error_code);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_FOE_SSC_H */
