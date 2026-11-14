/**
 * @file ecat_foe_ssc.h
 * @brief Beckhoff SSC (Slave Stack Code) 协议栈适配层接口定义
 *
 * 重要声明与边界限制：
 * 本模块为通用固件升级核心状态机接入 Beckhoff Slave Stack Code (SSC) 的薄转接层设计。
 * 本机当前缺乏真实 Beckhoff SSC 官方源码包（受 ETG 许可约束需官方会员账号导出），
 * 因此本模块尚未在真实 SSC 协议栈与物理硬件上进行联动测试。
 * 严禁将本模块的抽象契约测试宣称为真实 SSC 验证！
 */

#ifndef ECAT_FOE_SSC_H
#define ECAT_FOE_SSC_H

#include "ecat_foe_core.h"
#include "ecat_foe_transport.h"
#include "ecat_foe_ssc_async.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SSC 常见 FoE 错误码常量 (依据 ETG 标准 FoE 规范定义)
 */
#define ECAT_FOE_SSC_ERR_NOTFOUND        0x8001U /**< 文件未找到 / 项目不匹配 */
#define ECAT_FOE_SSC_ERR_NORIGHTS        0x8002U /**< 权限不足 / 拒绝访问 */
#define ECAT_FOE_SSC_ERR_DISKFULL        0x8003U /**< 存储空间已满 */
#define ECAT_FOE_SSC_ERR_ILLEGAL         0x8004U /**< 非法 FoE 操作 */
#define ECAT_FOE_SSC_ERR_PACKETNO        0x8005U /**< 包序号错误 */
#define ECAT_FOE_SSC_ERR_ALREADYEXISTS   0x8006U /**< 文件已存在 */
#define ECAT_FOE_SSC_ERR_NOUSER          0x8007U /**< 无此用户 */
#define ECAT_FOE_SSC_ERR_BOOTSTRAPONLY   0x8008U /**< 仅允许在 Bootstrap 态操作 */
#define ECAT_FOE_SSC_ERR_NOTINBOOTSTRAP  0x8009U /**< 当前非 Bootstrap 态 */
#define ECAT_FOE_SSC_ERR_NORIGHTS_PW     0x800AU /**< 密码错误 */
#define ECAT_FOE_SSC_ERR_PROGERROR       0x800BU /**< 编程/校验错误 */

/**
 * @brief SSC 适配层上下文结构体
 */
typedef struct {
    ecat_foe_transport_t transport;
    ecat_foe_core_t *core;                /**< 关联的核心机上下文 */
    ecat_foe_ssc_async_t *async_gate;      /**< 可选：异步 DATA/BUSY 门控 */
    uint32_t         max_packet_size;     /**< SSC 邮箱配置的单包最大容量 (若已知) */
    uint32_t         expected_password;    /**< 允许的 FoE 密码，默认 0 */
    bool             session_active;      /**< 当前会话是否处于激活状态 */
    bool             async_final_acked;   /**< 异步末包已完成并向 SSC 确认 */
} ecat_foe_ssc_adapter_t;

/**
 * @brief 初始化 SSC 适配层上下文
 * @param adapter 适配层上下文指针
 * @param core    核心机上下文指针
 * @param max_packet_size SSC 邮箱单包大小 (通常为邮箱数据载荷容量)
 */
void ecat_foe_ssc_init(ecat_foe_ssc_adapter_t *adapter, ecat_foe_core_t *core, uint32_t max_packet_size);
/* Bind once after init, before accepting requests. For async mode initialize
 * the gate with ecat_foe_transport_async_ops() and &adapter->transport first.
 * The shared manager must refer to this same core. */
int ecat_foe_ssc_bind_upgrade(ecat_foe_ssc_adapter_t *adapter, ecat_upgrade_t *upgrade);


/**
 * Set the FoE access password expected by this adapter. Defaults to zero after
 * init; configure this before accepting WRQ requests when using a nonzero value.
 * This is protocol-level access control, not cryptographic firmware authentication.
 */
int ecat_foe_ssc_set_expected_password(ecat_foe_ssc_adapter_t *adapter,
                                       uint32_t expected_password);

/** Bind an initialized asynchronous DATA gate while keeping WRQ validation in the core. */
void ecat_foe_ssc_init_async(ecat_foe_ssc_adapter_t *adapter,
                             ecat_foe_core_t *core,
                             ecat_foe_ssc_async_t *async_gate);

/**
 * @brief SSC FOE_Write 回调适配转接函数
 * 对应 SSC foeappl.c 中的 FOE_Write 入口。解析文件名并触发核心机建立升级会话。
 * @param adapter      适配层上下文
 * @param pName        文件名指针
 * @param nameSize     文件名长度 (字节)
 * @param password     FoE 访问密码
 * @return 0 成功；非 0 返回 FoE 协议错误码 (如 0x8001 / 0x800B)
 */
uint16_t ecat_foe_ssc_on_write(ecat_foe_ssc_adapter_t *adapter, const char *pName, uint16_t nameSize, uint32_t password);

/**
 * @brief SSC FOE_Data 回调适配转接函数
 * 对应 SSC foeappl.c 中的 FOE_Data 入口。递交分块数据至核心机进行装配、擦除与对齐写入。
 * @param adapter 适配层上下文
 * @param pData   接收到的数据块指针
 * @param length  当前块字节数 (若 length < max_packet_size 判定为短包/尾包)
 * @return 0 成功；非 0 返回 FoE 协议错误码 (如 0x800B)
 */
uint16_t ecat_foe_ssc_on_data(ecat_foe_ssc_adapter_t *adapter, const uint8_t *pData, uint16_t length);

/**
 * SSC callback variant for `pAPPL_FoeWriteData(pData, Size, bDataFollowing)`.
 * The application supplies monotonic 32-bit ticks; BUSY/retry codes come from
 * the initialized async gate. This is serialized with async_poll by contract.
 */
uint16_t ecat_foe_ssc_on_data_following(ecat_foe_ssc_adapter_t *adapter,
                                        const uint8_t *pData,
                                        uint16_t length,
                                        bool data_following,
                                        uint32_t now_ticks);

/** Advance one asynchronous operation step from the SSC application main loop. */
uint16_t ecat_foe_ssc_on_main_loop(ecat_foe_ssc_adapter_t *adapter,
                                   uint32_t now_ticks);

/**
 * @brief SSC 显式结束通知适配转接函数 (若具体 SSC 配置启用显式通知)
 * @param adapter 适配层上下文
 * @return 0 成功；非 0 失败
 */
uint16_t ecat_foe_ssc_on_end(ecat_foe_ssc_adapter_t *adapter);

/**
 * @brief SSC 中止/错误处理适配转接函数
 * 对应 SSC 中的错误发生或主站 Abort 时调用，重置会话并清理驱动。
 * @param adapter   适配层上下文
 * @param errorCode 错误代码
 */
void ecat_foe_ssc_on_abort(ecat_foe_ssc_adapter_t *adapter, uint16_t errorCode);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_FOE_SSC_H */
