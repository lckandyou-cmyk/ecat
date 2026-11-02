/**
 * @file ecat_foe_target.h
 * @brief EtherCAT FoE 目标驱动抽象接口定义
 *
 * 核心库不绑定任何具体 MCU 闪存控制器、FPGA 加载器或 EEPROM 驱动。
 * 驱动层通过实现此接口的操作函数集，并向核心库注册目标 ID。
 */

#ifndef ECAT_FOE_TARGET_H
#define ECAT_FOE_TARGET_H

#include "ecat_foe_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 目标升级驱动操作集
 */
typedef struct ecat_foe_target_ops {
    uint16_t target_id;       /**< 目标 ID (例如 ECAT_FOE_TARGET_APP) */
    const char *name;         /**< 目标类型字符串 ("APP", "FPGA", "EEPROM") */

    /**
     * @brief 升级初始化回调 (可选)
     * @param user_ctx 用户自定义上下文指针
     * @return 0 成功，非 0 失败
     */
    int (*init)(void *user_ctx);

    /**
     * @brief 擦除目标存储空间
     * @param offset 分区内相对偏移 (字节)
     * @param size   需擦除的总字节数
     * @param user_ctx 用户自定义上下文指针
     * @return 0 成功，非 0 失败
     */
    int (*erase)(uint32_t offset, uint32_t size, void *user_ctx);

    /**
     * @brief 写入固件数据块
     * @param offset 分区内相对偏移 (字节)
     * @param data   写入数据缓冲区
     * @param len    数据长度 (字节)
     * @param user_ctx 用户自定义上下文指针
     * @return 0 成功，非 0 失败
     */
    int (*write)(uint32_t offset, const uint8_t *data, uint32_t len, void *user_ctx);

    /**
     * @brief 固件校验回调 (可选，核心库在流式写入完成后计算并比对 CRC32，随后调用此回调)
     * @param total_size 写入的总有效负载大小
     * @param expected_crc 期望的 CRC32 值
     * @param user_ctx 用户自定义上下文指针
     * @return 0 校验成功，非 0 校验失败
     */
    int (*verify)(uint32_t total_size, uint32_t expected_crc, void *user_ctx);

    /**
     * @brief 固件升级提交流程 (标记固件就绪、切换启动向量或更新元数据)
     * @param success 是否全流程无误
     * @param user_ctx 用户自定义上下文指针
     * @return 0 成功，非 0 失败
     */
    int (*finalize)(bool success, void *user_ctx);

    /**
     * @brief 异常中止回调 (主站主动取消、校验失败或通信超时中断时触发清理)
     * @param user_ctx 用户自定义上下文指针
     */
    void (*abort)(void *user_ctx);
} ecat_foe_target_ops_t;

/**
 * @brief 目标分区配置项 (仅由 Bootloader 本地配置决定，不采信固件文件中携带的任何写入地址)
 */
typedef struct {
    uint16_t target_id;       /**< 目标 ID */
    bool     enabled;         /**< 是否开启该目标升级 */
    uint32_t base_addr;       /**< 物理起始基地址 */
    uint32_t max_size;        /**< 允许的最大固件大小 (字节) */
} ecat_foe_partition_t;

#ifdef __cplusplus
}
#endif

#endif /* ECAT_FOE_TARGET_H */
