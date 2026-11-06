/**
 * @file bootloader_master_flow.h
 * @brief EtherCAT FoE 主站发送交互流程与宿主测试参考接口
 *
 * 定义标准 EtherCAT 主站 (如 TwinCAT / SOEM / 自研主站)
 * 针对从站 Bootloader 发起 FoE 固件升级的标准交互时序与报文构造封装。
 */

#ifndef BOOTLOADER_MASTER_FLOW_H
#define BOOTLOADER_MASTER_FLOW_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "bootloader_app.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOOTLOADER_FOE_OP_RRQ  1U
#define BOOTLOADER_FOE_OP_WRQ  2U
#define BOOTLOADER_FOE_OP_DATA 3U
#define BOOTLOADER_FOE_OP_ACK  4U
#define BOOTLOADER_FOE_OP_ERR  5U
#define BOOTLOADER_FOE_OP_BUSY 6U

/**
 * @brief 主站接收到的 FoE 响应报文解析结果
 */
typedef struct {
    uint8_t  opcode;        /**< 响应操作码 (0x04 ACK, 0x05 ERR) */
    uint32_t packet_number; /**< ACK 对应的包号 */
    uint32_t error_code;    /**< ERR 对应的 32 位错误码 (如 0x8000, 0x800B) */
    uint8_t  mbxcnt;        /**< 信箱头部计数器 (高 4 位) */
} master_foe_response_t;

/**
 * @brief 构造合成标准 .efw 固件容器镜像 (包含 64 字节头部及有效载荷)
 *
 * @param out_buf       输出固件缓冲区
 * @param out_max       缓冲区最大容量
 * @param project_id    工程 ID (如 0x10010001)
 * @param project_name  工程名 (如 "SERVO_DRV")
 * @param hardware_id   硬件 ID (如 0x00000001)
 * @param hw_ver_min    最小兼容硬件版本
 * @param hw_ver_max    最大兼容硬件版本
 * @param target_id     目标存储 ID (如 1 为 MCU)
 * @param fw_ver        固件版本号 (如 0x01000000 为 1.0.0.0)
 * @param payload       有效固件载荷
 * @param payload_len   载荷字节数
 * @return 生成的 .efw 镜像总字节数，若缓冲区不足返回 0
 */
size_t bootloader_master_build_efw(uint8_t *out_buf,
                                   size_t out_max,
                                   uint32_t project_id,
                                   const char *project_name,
                                   uint32_t hardware_id,
                                   uint16_t hw_ver_min,
                                   uint16_t hw_ver_max,
                                   uint16_t target_id,
                                   uint32_t fw_ver,
                                   const uint8_t *payload,
                                   size_t payload_len);

/**
 * @brief 主站下发 FoE WRQ 写请求报文
 *
 * @param app           从站 Bootloader 上下文
 * @param foe_cfg       SOES foe_cfg 指针
 * @param filename      请求文件名 (符合 <NAME>_<TARGET>_V<M>.<m>.<p>.<b|r>.efw 规则)
 * @param password      文件密码 (通常为 0)
 * @param resp_out      输出主站接收到的从站响应
 * @return 0 通信成功且收到有效响应，非 0 失败
 */
int bootloader_master_send_wrq(bootloader_app_t *app,
                               foe_cfg_t *foe_cfg,
                               const char *filename,
                               uint32_t password,
                               master_foe_response_t *resp_out);

/**
 * @brief 主站下发 FoE DATA 数据报文
 *
 * @param app           从站 Bootloader 上下文
 * @param foe_cfg       SOES foe_cfg 指针
 * @param packet_number 数据包序号 (1, 2, ... N)
 * @param data          数据块首地址
 * @param data_len      数据块字节数
 * @param resp_out      输出主站接收到的从站响应
 * @return 0 通信成功且收到有效响应，非 0 失败
 */
int bootloader_master_send_data(bootloader_app_t *app,
                                foe_cfg_t *foe_cfg,
                                uint32_t packet_number,
                                const uint8_t *data,
                                size_t data_len,
                                master_foe_response_t *resp_out);

/**
 * @brief 主站下发 FoE ERR 异常中断/复位报文 (0x05)
 *
 * @param app           从站 Bootloader 上下文
 * @param foe_cfg       SOES foe_cfg 指针
 * @param error_code    错误码 (如 0x8000)
 * @return 0 成功
 */
int bootloader_master_send_err(bootloader_app_t *app,
                               foe_cfg_t *foe_cfg,
                               uint32_t error_code);

#ifdef __cplusplus
}
#endif

#endif /* BOOTLOADER_MASTER_FLOW_H */
