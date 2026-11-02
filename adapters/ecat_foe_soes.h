/**
 * @file ecat_foe_soes.h
 * @brief Simple Open EtherCAT Slave (SOES) FoE 适配转接层
 *
 * 为 SOES (Simple Open EtherCAT Slave) 协议栈提供薄转接接口。
 * 兼容 SOES 的 foe_file_cfg_t 回调约定。
 */

#ifndef ECAT_FOE_SOES_H
#define ECAT_FOE_SOES_H

#include "ecat_foe.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SOES foe_write 回调函数
 *
 * @param self 对应 SOES 的 foe_file_cfg_t 结构体指针
 * @param name 文件名
 * @param name_len 文件名长度
 * @param password 访问密码
 * @return 0 成功，非 0 返回 FoE 错误码
 */
uint16_t ecat_foe_soes_write(void *self, uint8_t *name, uint32_t name_len, uint32_t password);

/**
 * @brief SOES foe_write_data 回调函数
 *
 * @param self 对应 SOES 的 foe_file_cfg_t 结构体指针
 * @param data 数据缓冲区
 * @param length 数据字节数
 * @param is_last 是否为末尾包 (1 为末包，0 为中间包)
 * @return 0 成功，非 0 返回 FoE 错误码
 */
uint16_t ecat_foe_soes_write_data(void *self, uint8_t *data, size_t length, uint8_t is_last);

/**
 * @brief SOES foe_read 回调函数
 */
uint16_t ecat_foe_soes_read(void *self, uint8_t *name, uint32_t name_len, uint32_t password);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_FOE_SOES_H */
