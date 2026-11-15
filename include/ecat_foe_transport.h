/**
 * @file ecat_foe_transport.h
 * @brief FoE 固件升级专用适配桥 (FoE Bridge into Firmware Upgrade Manager)
 *
 * 【架构分层与迁移边界契约】：
 * - 本组件为既有 EtherCAT FoE 协议层至固件升级管理器 (ecat_upgrade_t) 的专用桥接器；
 * - 互斥与仲裁保证：通过 upgrade->session_ptr 共享 ecat_upgrade_session_t，与统一传输引擎
 *   (ecat_transfer_t) 在底层会话租约层完全互斥排他；
 * - 分阶段迁移边界：当前阶段本适配器保持既有轻量状态机与 ecat_upgrade_t 绑定，不直接并入
 *   ecat_transfer_t，以确保对原有 Beckhoff SSC / SOES 协议栈及测试套件的 100% 行为兼容；
 * - 若需多目标双向通用数据传输，请使用统一传输端点 ecat_transport_endpoint_t。
 */
#ifndef ECAT_FOE_TRANSPORT_H
#define ECAT_FOE_TRANSPORT_H
#include "ecat_upgrade.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ecat_upgrade_t *upgrade;
    uint64_t token;
    bool pending;
    bool finish_after_write;
    bool finishing;
    bool committed;
} ecat_foe_transport_t;

/* Initialize once; address is the stable owner identity, so do not copy it. */
int ecat_foe_transport_init(ecat_foe_transport_t *transport, ecat_upgrade_t *upgrade);
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
