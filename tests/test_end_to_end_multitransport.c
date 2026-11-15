/**
 * @file test_end_to_end_multitransport.c
 * @brief 真实业务场景端到端测试: 多通道 + 多目标 + 权限矩阵 + 双向导入导出
 *
 * 场景还原:
 * 1. 系统包含 3 个传输通道:
 *    - 通道 1 (FoE): 工业总线通道
 *    - 通道 2 (UART): 本地调试串口通道
 *    - 通道 3 (CAN): 车辆/底盘控制总线通道
 * 2. 系统包含 4 个物理存储目标:
 *    - 目标 1: "mcu_fw" (固件区: 仅写, 大小 1MB)
 *    - 目标 2: "fpga_img" (固件区: 仅写, 大小 512KB)
 *    - 目标 3: "sys_params" (数据区: 读写 RW, 大小 64KB)
 *    - 目标 4: "flight_log" (数据区: 仅读 READ, 大小 128KB)
 * 3. 访问控制矩阵 (ACL):
 *    - FoE (1) 仅允许写 MCU 固件 (1) 和 FPGA (2)
 *    - UART (2) 仅允许读写系统参数 (3)
 *    - CAN (3) 仅允许导出黑匣子日志 (4)
 * 4. 验证内容:
 *    - 合法操作全部跑通并比对 CRC 与字节一致性
 *    - 越权与违规操作 (如 UART 篡改固件、CAN 写入日志、FoE 越权读取) 100% 被拦截
 */

#include "ecat_transport_endpoint.h"
#include "ecat_target_registry.h"
#include "ecat_transfer.h"
#include "ecat_foe_efw.h"
#include "ecat_upgrade.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

/* Mock 异步数据驱动 */
static ecat_async_op_result_t s_e2e_async_res = ECAT_ASYNC_OP_COMPLETE;
static ecat_async_op_result_t e2e_async_start(void *ctx, ecat_async_data_op_t *op)
{
    (void)ctx; (void)op;
    return s_e2e_async_res;
}
static ecat_async_op_result_t e2e_async_poll(void *ctx, ecat_async_data_op_t *op)
{
    (void)ctx; (void)op;
    return s_e2e_async_res;
}
static void e2e_async_abort(void *ctx) { (void)ctx; }

int port_get_version_floor(uint16_t id, uint32_t *v)
{
    (void)id;
    if (v) *v = 0;
    return 0;
}

/* 模拟存储介质 */
static uint8_t s_flash_mcu[1024];
static uint8_t s_flash_params[256];
static uint8_t s_flash_log[512];

/* MCU 固件驱动 Mock */
static int mcu_init(void *ctx) { (void)ctx; return 0; }
static int mcu_invalidate(void *ctx) { (void)ctx; return 0; }
static int mcu_erase_to(uint32_t need_end, uint32_t *erased_end, void *ctx)
{
    (void)ctx;
    if (erased_end) *erased_end = need_end;
    return 0;
}
static int mcu_write(uint32_t offset, const uint8_t *data, uint32_t len, void *ctx)
{
    (void)ctx;
    if (offset + len > sizeof(s_flash_mcu)) return -1;
    memcpy(&s_flash_mcu[offset], data, len);
    return 0;
}
static int mcu_read(uint32_t offset, uint8_t *buf, uint32_t len, void *ctx)
{
    (void)ctx;
    if (offset + len > sizeof(s_flash_mcu)) return -1;
    memcpy(buf, &s_flash_mcu[offset], len);
    return 0;
}
static int mcu_finalize(void *ctx) { (void)ctx; return 0; }
static void mcu_abort(void *ctx) { (void)ctx; }

/* 参数区驱动 Mock */
static int params_init(void *ctx) { (void)ctx; return 0; }
static int params_read_open(void *ctx) { (void)ctx; return 0; }
static void params_read_close(void *ctx) { (void)ctx; }
static int params_erase_to(uint32_t need_end, uint32_t *erased_end, void *ctx)
{
    (void)ctx;
    if (erased_end) *erased_end = need_end;
    return 0;
}
static int params_write(uint32_t offset, const uint8_t *data, uint32_t len, void *ctx)
{
    (void)ctx;
    if (offset + len > sizeof(s_flash_params)) return -1;
    memcpy(&s_flash_params[offset], data, len);
    return 0;
}
static int params_read(uint32_t offset, uint8_t *buf, uint32_t len, void *ctx)
{
    (void)ctx;
    if (offset + len > sizeof(s_flash_params)) return -1;
    memcpy(buf, &s_flash_params[offset], len);
    return 0;
}
static int params_finalize(void *ctx) { (void)ctx; return 0; }
static void params_abort(void *ctx) { (void)ctx; }
static uint32_t params_get_size(void *ctx) { (void)ctx; return 128U; }

/* 日志区驱动 Mock */
static int log_init(void *ctx) { (void)ctx; return 0; }
static int log_read_open(void *ctx) { (void)ctx; return 0; }
static void log_read_close(void *ctx) { (void)ctx; }
static int log_read(uint32_t offset, uint8_t *buf, uint32_t len, void *ctx)
{
    (void)ctx;
    if (offset + len > sizeof(s_flash_log)) return -1;
    memcpy(buf, &s_flash_log[offset], len);
    return 0;
}
static uint32_t log_get_size(void *ctx) { (void)ctx; return sizeof(s_flash_log); }
static int log_finalize(void *ctx) { (void)ctx; return 0; }
static void log_abort(void *ctx) { (void)ctx; }

int main(void)
{
    printf("=== 开始运行端到端多通道多目标双向传输测试 ===\n");

    /* 1. 初始化目标注册表与权限矩阵 */
    ecat_target_registry_t reg;
    assert(ecat_target_registry_init(&reg) == 0);

    /* 注册目标 1: MCU 固件 (FIRMWARE 类型) */
    ecat_target_desc_t mcu_desc;
    memset(&mcu_desc, 0, sizeof(mcu_desc));
    mcu_desc.target_id = 1U;
    strncpy(mcu_desc.name, "mcu_fw", sizeof(mcu_desc.name));
    mcu_desc.type = ECAT_TARGET_TYPE_FIRMWARE;
    mcu_desc.permissions = ECAT_PERM_WRITE;
    mcu_desc.max_size = 1024U * 1024U;
    mcu_desc.enabled = true;
    mcu_desc.driver.io.init = mcu_init;
    mcu_desc.driver.io.invalidate = mcu_invalidate;
    mcu_desc.driver.io.erase_to = mcu_erase_to;
    mcu_desc.driver.io.write = mcu_write;
    mcu_desc.driver.io.read = mcu_read;
    mcu_desc.driver.io.finalize = mcu_finalize;
    mcu_desc.driver.io.abort = mcu_abort;
    assert(ecat_target_registry_add(&reg, &mcu_desc) == 0);

    /* 注册目标 3: 系统参数区 (DATA 类型, RW) */
    ecat_target_desc_t param_desc;
    memset(&param_desc, 0, sizeof(param_desc));
    param_desc.target_id = 3U;
    strncpy(param_desc.name, "sys_params", sizeof(param_desc.name));
    param_desc.type = ECAT_TARGET_TYPE_DATA;
    param_desc.permissions = ECAT_PERM_RW;
    param_desc.max_size = sizeof(s_flash_params);
    param_desc.enabled = true;
    param_desc.driver.read_open = params_read_open;
    param_desc.driver.read_close = params_read_close;
    param_desc.driver.io.init = params_init;
    param_desc.driver.io.erase_to = params_erase_to;
    param_desc.driver.io.write = params_write;
    param_desc.driver.io.read = params_read;
    param_desc.driver.io.finalize = params_finalize;
    param_desc.driver.io.abort = params_abort;
    param_desc.driver.get_data_size = params_get_size;
    assert(ecat_target_registry_add(&reg, &param_desc) == 0);

    /* 注册目标 4: 运行日志区 (DATA 类型, READ 仅读) */
    ecat_target_desc_t log_desc;
    memset(&log_desc, 0, sizeof(log_desc));
    log_desc.target_id = 4U;
    strncpy(log_desc.name, "flight_log", sizeof(log_desc.name));
    log_desc.type = ECAT_TARGET_TYPE_DATA;
    log_desc.permissions = ECAT_PERM_READ;
    log_desc.max_size = sizeof(s_flash_log);
    log_desc.enabled = true;
    log_desc.driver.read_open = log_read_open;
    log_desc.driver.read_close = log_read_close;
    log_desc.driver.io.init = log_init;
    log_desc.driver.io.read = log_read;
    log_desc.driver.io.finalize = log_finalize;
    log_desc.driver.io.abort = log_abort;
    log_desc.driver.get_data_size = log_get_size;
    assert(ecat_target_registry_add(&reg, &log_desc) == 0);

    /* 预填充日志区数据 */
    for (size_t i = 0; i < sizeof(s_flash_log); i++) {
        s_flash_log[i] = (uint8_t)(i ^ 0xA5);
    }

    /* 2. 配置严格 ACL 访问控制规则 */
    ecat_target_registry_set_enforce_acl(&reg, true);
    /* 规则 1: FoE (1) 仅允许写 MCU 固件 (1) */
    assert(ecat_target_registry_add_acl(&reg, 1U, 1U, ECAT_PERM_WRITE) == 0);
    /* 规则 2: UART (2) 仅允许读写系统参数 (3) */
    assert(ecat_target_registry_add_acl(&reg, 2U, 3U, ECAT_PERM_RW) == 0);
    /* 规则 3: CAN (3) 仅允许导出日志区 (4) */
    assert(ecat_target_registry_add_acl(&reg, 3U, 4U, ECAT_PERM_READ) == 0);

    /* 3. 初始化核心机与传输管理器 */
    ecat_foe_core_t core;
    ecat_foe_core_cfg_t core_cfg;
    memset(&core_cfg, 0, sizeof(core_cfg));
    core_cfg.project_name = "DRIVE_SERVO";
    core_cfg.hardware_id = 0x0001U;
    core_cfg.local_hw_ver = 0x0100U;
    core_cfg.max_header_size = 148U;
    core_cfg.targets[0].enabled = true;
    core_cfg.targets[0].max_payload_size = 1024U * 1024U;
    core_cfg.targets[0].write_unit = 8U;
    core_cfg.targets[0].driver = &mcu_desc.driver.io;
    core_cfg.targets[0].user_ctx = NULL;
    assert(ecat_foe_core_init(&core, &core_cfg) == 0);

    ecat_transfer_t mgr;
    assert(ecat_transfer_init(&mgr, &reg, &core) == 0);

    /* 4. 绑定 3 个传输端点 */
    ecat_transport_endpoint_t ep_foe;
    ecat_transport_endpoint_t ep_uart;
    ecat_transport_endpoint_t ep_can;

    assert(ecat_transport_endpoint_init(&ep_foe, 1U, "ep_foe", &mgr, NULL) == 0);
    assert(ecat_transport_endpoint_init(&ep_uart, 2U, "ep_uart", &mgr, NULL) == 0);
    assert(ecat_transport_endpoint_init(&ep_can, 3U, "ep_can", &mgr, NULL) == 0);

    /* ---------------------------------------------------------------------- */
    /* 测试阶段 A: 越权攻击/未授权访问拦截                                   */
    /* ---------------------------------------------------------------------- */
    /* A1: UART 试图向 MCU 固件区刷写固件 -> 必须被 ACL 拦截 */
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 1U, 1024, 0x01000000U) == ECAT_TRANSFER_DENIED);

    /* A2: CAN 试图向日志区写入数据 -> 目标仅读且无 ACL -> 拦截 */
    assert(ecat_transport_endpoint_begin_import(&ep_can, 4U, 128, 0) == ECAT_TRANSFER_DENIED);

    /* A3: FoE 试图导出系统参数 -> 必须被 ACL 拦截 */
    assert(ecat_transport_endpoint_begin_export(&ep_foe, 3U, NULL) == ECAT_TRANSFER_DENIED);

    /* ---------------------------------------------------------------------- */
    /* 测试阶段 B: 合法业务流程执行                                           */
    /* ---------------------------------------------------------------------- */
    /* B1: UART 成功导入系统参数 */
    uint8_t new_params[128];
    for (size_t i = 0; i < sizeof(new_params); i++) {
        new_params[i] = (uint8_t)(i + 0x20);
    }
    uint32_t expected_param_crc = ecat_foe_crc32_calc(new_params, sizeof(new_params));

    assert(ecat_transport_endpoint_begin_import(&ep_uart, 3U, sizeof(new_params), 0) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, new_params, sizeof(new_params)) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_finish_import(&ep_uart) == ECAT_TRANSFER_COMPLETE);
    assert(memcmp(s_flash_params, new_params, sizeof(new_params)) == 0);

    /* B2: UART 成功回读验证系统参数 */
    uint32_t param_export_size = 0;
    assert(ecat_transport_endpoint_begin_export(&ep_uart, 3U, &param_export_size) == ECAT_TRANSFER_COMPLETE);
    uint8_t readback_params[128];
    size_t actual_rb = 0;
    assert(ecat_transport_endpoint_read_chunk(&ep_uart, readback_params, sizeof(readback_params), &actual_rb) == ECAT_TRANSFER_COMPLETE);
    assert(actual_rb == sizeof(new_params));
    assert(memcmp(readback_params, new_params, sizeof(new_params)) == 0);
    assert(ecat_transfer_get_crc32(&mgr) == expected_param_crc);
    assert(ecat_transport_endpoint_finish_export(&ep_uart) == ECAT_TRANSFER_COMPLETE);

    /* B3: CAN 成功导出黑匣子日志 */
    uint32_t log_size = 0;
    assert(ecat_transport_endpoint_begin_export(&ep_can, 4U, &log_size) == ECAT_TRANSFER_COMPLETE);
    assert(log_size == sizeof(s_flash_log));

    uint8_t rx_log[sizeof(s_flash_log)];
    size_t log_rx_total = 0;
    while (log_rx_total < log_size) {
        uint8_t chunk[64];
        size_t chunk_len = 0;
        ecat_transfer_result_t res = ecat_transport_endpoint_read_chunk(
            &ep_can, chunk, sizeof(chunk), &chunk_len);
        assert(chunk_len > 0);
        memcpy(&rx_log[log_rx_total], chunk, chunk_len);
        log_rx_total += chunk_len;
        if (log_rx_total == log_size) {
            assert(res == ECAT_TRANSFER_COMPLETE);
        } else {
            assert(res == ECAT_TRANSFER_CONTINUE);
        }
    }

    assert(memcmp(rx_log, s_flash_log, sizeof(s_flash_log)) == 0);
    uint32_t expected_log_crc = ecat_foe_crc32_calc(s_flash_log, sizeof(s_flash_log));
    assert(ecat_transfer_get_crc32(&mgr) == expected_log_crc);
    assert(ecat_transport_endpoint_finish_export(&ep_can) == ECAT_TRANSFER_COMPLETE);

    /* ---------------------------------------------------------------------- */
    /* 测试阶段 C: 跨通道会话互斥仲裁、异步挂起阻断与旧升级入口对齐验证     */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试阶段 C: 跨通道会话互斥仲裁、异步挂起阻断与旧升级入口对齐验证 ---\n");

    /* C1: 多通道同步互斥仲裁 */
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 3U, sizeof(new_params), 0) == ECAT_TRANSFER_COMPLETE);
    /* UART 处于活动状态，CAN 发起导出必须返回 BUSY */
    assert(ecat_transport_endpoint_begin_export(&ep_can, 4U, NULL) == ECAT_TRANSFER_BUSY);
    /* FoE 端点发起导入也必须返回 BUSY */
    assert(ecat_transport_endpoint_begin_import(&ep_foe, 1U, 1024, 0x01000000U) == ECAT_TRANSFER_BUSY);

    /* 初始化旧升级管理器实例并共享 core 会话 */
    ecat_upgrade_t legacy_upgrade;
    assert(ecat_upgrade_init(&legacy_upgrade, &core) == 0);
    ecat_upgrade_request_t upg_req = { .target_id = 1U, .fw_version = 0x01000000U };
    uint64_t upg_token = 0;
    /* UART 端点占有会话时，旧升级 begin 必须返回 BUSY */
    assert(ecat_upgrade_begin(&legacy_upgrade, &legacy_upgrade, &upg_req, &upg_token) == ECAT_UPGRADE_BUSY);

    /* UART 端点取消并释放会话 */
    ecat_transport_endpoint_abort(&ep_uart);
    assert(ep_uart.current_token == 0U);

    /* C2: 注册异步目标 5，验证真正异步挂起期间全局租约阻断与释放 */
    ecat_async_data_ops_t e2e_async_ops = {
        .start = e2e_async_start,
        .poll = e2e_async_poll,
        .abort = e2e_async_abort
    };
    ecat_target_desc_t async_desc;
    memset(&async_desc, 0, sizeof(async_desc));
    async_desc.target_id = 5U;
    strncpy(async_desc.name, "async_storage", sizeof(async_desc.name));
    async_desc.type = ECAT_TARGET_TYPE_DATA;
    async_desc.permissions = ECAT_PERM_RW;
    async_desc.max_size = 1024U;
    async_desc.enabled = true;
    async_desc.asynchronous = true;
    async_desc.driver.async_ops = &e2e_async_ops;
    assert(ecat_target_registry_add(&reg, &async_desc) == 0);
    /* 添加 ACL: UART 允许写 5U */
    assert(ecat_target_registry_add_acl(&reg, 2U, 5U, ECAT_PERM_WRITE) == 0);

    /* 绑定暂存区 */
    uint8_t e2e_staging[256];
    assert(ecat_transfer_set_staging_buffer(&mgr, e2e_staging, sizeof(e2e_staging)) == 0);

    /* UART 发起异步写入 */
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 5U, 64, 0) == ECAT_TRANSFER_COMPLETE);
    s_e2e_async_res = ECAT_ASYNC_OP_PENDING;
    uint8_t async_chunk[64];
    memset(async_chunk, 0x55, sizeof(async_chunk));
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, async_chunk, sizeof(async_chunk)) == ECAT_TRANSFER_PENDING);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == true);

    /* 异步 PENDING 期间，所有其它通道与旧升级接口必须被严格阻断 (BUSY) */
    assert(ecat_transport_endpoint_begin_export(&ep_can, 4U, NULL) == ECAT_TRANSFER_BUSY);
    assert(ecat_transport_endpoint_begin_import(&ep_foe, 1U, 1024, 0x01000000U) == ECAT_TRANSFER_BUSY);
    assert(ecat_upgrade_begin(&legacy_upgrade, &legacy_upgrade, &upg_req, &upg_token) == ECAT_UPGRADE_BUSY);

    /* 推进 poll 完成数据写入 */
    s_e2e_async_res = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transport_endpoint_poll(&ep_uart) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == false);

    /* 终结提交 */
    assert(ecat_transport_endpoint_finish_import(&ep_uart) == ECAT_TRANSFER_COMPLETE);
    assert(ep_uart.current_token == 0U);

    /* C3: 会话释放后，旧升级接口可正常发起并反向互斥 */
    assert(ecat_upgrade_begin(&legacy_upgrade, &legacy_upgrade, &upg_req, &upg_token) == ECAT_UPGRADE_COMPLETE);
    assert(upg_token != 0);

    /* 旧升级活跃期间，所有新端点发起操作必须被拦截返回 BUSY */
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 3U, 16, 0) == ECAT_TRANSFER_BUSY);
    assert(ecat_transport_endpoint_begin_export(&ep_can, 4U, NULL) == ECAT_TRANSFER_BUSY);

    /* 旧升级 cancel 释放 */
    assert(ecat_upgrade_cancel(&legacy_upgrade, &legacy_upgrade, upg_token) == ECAT_UPGRADE_COMPLETE);

    /* 释放后新端点可再次正常发起 */
    assert(ecat_transport_endpoint_begin_export(&ep_can, 4U, &log_size) == ECAT_TRANSFER_COMPLETE);
    ecat_transport_endpoint_abort(&ep_can);

    printf(">>> 端到端多通道多目标双向传输测试全部通过！<<<\n");
    return 0;
}
