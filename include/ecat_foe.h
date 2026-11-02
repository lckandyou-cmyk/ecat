/**
 * @file ecat_foe.h
 * @brief EtherCAT FoE 通用固件升级核心库主接口
 *
 * 纯 C99 实现，零动态内存分配 (无 malloc/free)，无外部芯片或协议栈依赖。
 */

#ifndef ECAT_FOE_H
#define ECAT_FOE_H

#include "ecat_foe_def.h"
#include "ecat_foe_target.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FoE 内部有限状态机状态
 */
typedef enum {
    ECAT_FOE_STATE_IDLE = 0,         /**< 空闲，等待主站下发 WRQ */
    ECAT_FOE_STATE_WRQ_ACKED,        /**< WRQ 文件名校验通过，等待第一个数据包 (含 .efw 头) */
    ECAT_FOE_STATE_HEADER_PARSED,    /**< 头部校验通过，目标存储已擦除，准备接收有效负载 */
    ECAT_FOE_STATE_RECEIVING,        /**< 正在流式接收并写入固件有效负载 */
    ECAT_FOE_STATE_VERIFYING,        /**< 数据接收完毕，执行整体验证 */
    ECAT_FOE_STATE_COMPLETED,        /**< 升级全部成功结束 */
    ECAT_FOE_STATE_ERROR             /**< 发生异常或被主站中止 */
} ecat_foe_state_t;

/**
 * @brief 核心库全局静态配置结构体
 */
typedef struct {
    uint32_t project_id;             /**< 预期工程 ID */
    const char *project_name;        /**< 预期工程标识串 */
    uint32_t hardware_id;            /**< 预期硬件版本 ID */

    /* 分区与使能定义 (最多支持 4 种升级目标) */
    ecat_foe_partition_t partitions[4];
    uint8_t partition_count;
} ecat_foe_config_t;

/**
 * @brief 初始化 FoE 核心升级库
 * @param cfg 静态配置指针
 * @return 0 成功，非 0 失败
 */
int ecat_foe_init(const ecat_foe_config_t *cfg);

/**
 * @brief 注册目标驱动操作集
 * @param ops 驱动操作函数表
 * @param user_ctx 传递给驱动回调的上下文指针
 * @return 0 成功，非 0 失败
 */
int ecat_foe_register_target(const ecat_foe_target_ops_t *ops, void *user_ctx);

/**
 * @brief 处理主站发起的 FoE 写请求 (WRQ)
 * 提取并校验文件名规则：<PROJECT>_<TARGET>_<VERSION>.efw
 *
 * @param filename 文件名字符串
 * @param filename_len 文件名长度
 * @param password 访问密码 (若协议栈支持)
 * @param[out] err_code 失败时的标准 FoE 错误码
 * @return true 接受请求，false 拒绝请求 (查看 err_code)
 */
bool ecat_foe_on_wrq(const char *filename, uint32_t filename_len, uint32_t password, uint16_t *err_code);

/**
 * @brief 处理主站发送的 FoE 数据包 (DATA)
 * 自动识别首包并解析校验 .efw 文件头，流式写入驱动，并在尾包执行最终验证
 *
 * @param packet_no 数据包序号 (通常从 1 开始累加)
 * @param data 数据缓冲区指针
 * @param len 数据长度
 * @param is_last 是否为最后一包数据
 * @param[out] err_code 失败时的标准 FoE 错误码
 * @return true 写入处理成功，false 处理失败
 */
bool ecat_foe_on_data(uint32_t packet_no, const uint8_t *data, uint32_t len, bool is_last, uint16_t *err_code);

/**
 * @brief 处理主站发起的 FoE 读请求 (RRQ)
 * @return true 允许读取，false 拒绝
 */
bool ecat_foe_on_rrq(const char *filename, uint32_t filename_len, uint32_t password, uint16_t *err_code);

/**
 * @brief 主动中止当前传输流程并重置状态机
 * @param err_code 错误码
 */
void ecat_foe_abort(uint16_t err_code);

/**
 * @brief 复位状态机为空闲状态
 */
void ecat_foe_reset(void);

/**
 * @brief 查询当前核心状态机状态
 */
ecat_foe_state_t ecat_foe_get_state(void);

/**
 * @brief 获取当前升级进度
 * @param[out] written_payload 已写入有效负载字节数
 * @param[out] total_payload   总固件有效负载字节数
 */
void ecat_foe_get_progress(uint32_t *written_payload, uint32_t *total_payload);

/**
 * @brief 获取当前正在操作的目标 ID
 */
uint16_t ecat_foe_get_active_target(void);

/**
 * @brief 标准 IEEE 802.3 CRC32 纯 C 增量计算辅助函数
 * @param init_crc 初始 CRC 值 (首轮传入 0xFFFFFFFFU)
 * @param data 数据缓冲区
 * @param len 字节数
 * @return 计算结果 (最后经 ~ 异或)
 */
uint32_t ecat_crc32_calc(uint32_t init_crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_FOE_H */
