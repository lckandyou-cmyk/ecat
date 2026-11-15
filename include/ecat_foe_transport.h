/**
 * @file ecat_foe_transport.h
 * @brief FoE 固件升级专用适配桥 (FoE Bridge into Firmware Upgrade Manager & Unified Transfer Core)
 *
 * 【架构分层与迁移边界契约】：
 * - 本组件为既有 EtherCAT FoE 协议层至底层传输状态机的适配桥接器；
 * - 统一核心原生接入：支持通过 ecat_foe_transport_init_transfer 直接接入权威统一双向传输核心
 *   (ecat_transfer_t)，此时 FoE 协议栈完全受管于统一状态机、统一 ACL 矩阵与生命周期；
 * - 向后兼容边界：保留通过 ecat_foe_transport_init 接入原有 ecat_upgrade_t 的兼容路径，此时两套
 *   管理器在底层共享同一会话仲裁器 (ecat_upgrade_session_t) 与 core，在租约层强绑定并完全排他互斥；
 * - 若需通用多目标双向流传输，推荐使用统一传输端点 ecat_transport_endpoint_t。
 */
#ifndef ECAT_FOE_TRANSPORT_H
#define ECAT_FOE_TRANSPORT_H
#include "ecat_upgrade.h"
#include "ecat_transfer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ecat_upgrade_t *upgrade;      /**< 遗留升级管理器 (兼容模式) */
    ecat_transfer_t *transfer;    /**< 权威统一双向传输管理器 (统一核心模式) */
    uint64_t token;
    bool pending;
    bool begin_pending;           /**< 标记当前正在进行 begin 异步等待 */
    bool finish_after_write;
    bool finishing;
    bool committed;
    const uint8_t *staged_data;   /**< 首包异步暂存数据指针 */
    size_t staged_length;         /**< 首包异步暂存数据长度 */
    bool staged_more_data;        /**< 首包后续数据标志 */
    bool has_staged_packet;       /**< 是否包含暂存的首包数据 */
} ecat_foe_transport_t;

/* Initialize once; address is the stable owner identity, so do not copy it. */
int ecat_foe_transport_init(ecat_foe_transport_t *transport, ecat_upgrade_t *upgrade);

/**
 * @brief 使用权威统一双向传输核心初始化 FoE 适配桥
 * @param transport 适配桥结构体指针
 * @param transfer 权威统一双向传输管理器
 * @return 0 成功, -1 参数非法
 */
int ecat_foe_transport_init_transfer(ecat_foe_transport_t *transport, ecat_transfer_t *transfer);

/* Returns FoE wire codes; a new WRQ can replace only this bridge's session. */
int ecat_foe_transport_begin(ecat_foe_transport_t *transport, const char *name, size_t length);
ecat_upgrade_async_result_t ecat_foe_transport_start(void *ctx, const uint8_t *data,
    size_t length, bool more_data);
ecat_upgrade_async_result_t ecat_foe_transport_poll(void *ctx);
void ecat_foe_transport_abort(void *ctx);
bool ecat_foe_transport_owns(const ecat_foe_transport_t *transport);
/* Suitable for the existing SSC retry gate; final packet waits for commit. */
const ecat_upgrade_async_ops_t *ecat_foe_transport_async_ops(void);
#ifdef __cplusplus
}
#endif
#endif
