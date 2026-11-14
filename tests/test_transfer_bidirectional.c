/**
 * @file test_transfer_bidirectional.c
 * @brief 双向数据传输 (导入与导出) 引擎全功能单元测试
 */

#include "ecat_transfer.h"
#include "ecat_upgrade.h"
#include "ecat_foe_transport.h"
#include "ecat_upgrade_file.h"
#include "ecat_foe_efw.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

#define MOCK_PARAM_SIZE 4096U
#define MOCK_LOG_SIZE   1234U

int port_get_version_floor(uint16_t id, uint32_t *v)
{
    (void)id;
    if (v) *v = 0;
    return 0;
}

static uint8_t g_mock_param_flash[MOCK_PARAM_SIZE];
static uint8_t g_mock_log_flash[MOCK_LOG_SIZE];

static bool g_init_called = false;
static bool g_invalidate_called = false;
static bool g_finalize_called = false;
static bool g_abort_called = false;
static uint32_t g_erased_bytes = 0U;

static bool g_read_open_called = false;
static bool g_read_close_called = false;
static int g_valid_state = 0;
static int g_driver_locked = 0;
static bool g_fail_read = false;
static bool g_fail_init = false;

static void reset_mock_flags(void)
{
    g_init_called = false;
    g_invalidate_called = false;
    g_finalize_called = false;
    g_abort_called = false;
    g_erased_bytes = 0U;
    g_read_open_called = false;
    g_read_close_called = false;
    g_valid_state = 0;
    g_driver_locked = 0;
    g_fail_read = false;
    g_fail_init = false;
}

static int lifecycle_read_open(void *ctx)
{
    (void)ctx;
    g_read_open_called = true;
    if (g_fail_init) return -1;
    g_driver_locked = 1;
    return 0;
}

static void lifecycle_read_close(void *ctx)
{
    (void)ctx;
    g_read_close_called = true;
    g_driver_locked = 0;
}

static int lifecycle_read(uint32_t offset, uint8_t *buf, uint32_t len, void *ctx)
{
    (void)ctx;
    if (g_fail_read) return -1;
    if (offset + len > MOCK_PARAM_SIZE) return -1;
    memcpy(buf, &g_mock_param_flash[offset], len);
    return 0;
}

static int lifecycle_init(void *ctx)
{
    (void)ctx;
    g_init_called = true;
    if (g_fail_init) return -1;
    g_driver_locked = 1;
    return 0;
}

static int lifecycle_finalize(void *ctx)
{
    (void)ctx;
    g_finalize_called = true;
    g_valid_state = 1;
    g_driver_locked = 0;
    return 0;
}

static void lifecycle_abort(void *ctx)
{
    (void)ctx;
    g_abort_called = true;
    g_valid_state = 0;
    g_driver_locked = 0;
}

/* Param Flash Mock 驱动 */
static int param_init(void *ctx) { (void)ctx; g_init_called = true; return 0; }
static int param_invalidate(void *ctx) { (void)ctx; g_invalidate_called = true; return 0; }
static int param_erase_to(uint32_t need_end, uint32_t *erased_end, void *ctx)
{
    (void)ctx;
    g_erased_bytes = need_end;
    if (erased_end) *erased_end = need_end;
    return 0;
}
static int param_write(uint32_t offset, const uint8_t *data, uint32_t len, void *ctx)
{
    (void)ctx;
    if (offset + len > MOCK_PARAM_SIZE) return -1;
    memcpy(&g_mock_param_flash[offset], data, len);
    return 0;
}
static int param_read(uint32_t offset, uint8_t *buf, uint32_t len, void *ctx)
{
    (void)ctx;
    if (offset + len > MOCK_PARAM_SIZE) return -1;
    memcpy(buf, &g_mock_param_flash[offset], len);
    return 0;
}
static int param_finalize(void *ctx) { (void)ctx; g_finalize_called = true; return 0; }
static void param_abort(void *ctx) { (void)ctx; g_abort_called = true; }

/* Log Mock 驱动 */
static int log_init(void *ctx) { (void)ctx; g_init_called = true; return 0; }
static int log_read(uint32_t offset, uint8_t *buf, uint32_t len, void *ctx)
{
    (void)ctx;
    if (offset + len > MOCK_LOG_SIZE) return -1;
    memcpy(buf, &g_mock_log_flash[offset], len);
    return 0;
}
static uint32_t log_get_size(void *ctx) { (void)ctx; return MOCK_LOG_SIZE; }
static int log_finalize(void *ctx) { (void)ctx; g_finalize_called = true; return 0; }
static void log_abort(void *ctx) { (void)ctx; g_abort_called = true; }

int main(void)
{
    printf("=== 开始运行双向数据传输 (导入与导出) 引擎单元测试 ===\n");

    ecat_target_registry_t reg;
    assert(ecat_target_registry_init(&reg) == 0);

    /* 注册 Target 2: 参数区 (RW) */
    ecat_target_desc_t param_desc;
    memset(&param_desc, 0, sizeof(param_desc));
    param_desc.target_id = 2U;
    strncpy(param_desc.name, "params", sizeof(param_desc.name));
    param_desc.type = ECAT_TARGET_TYPE_DATA;
    param_desc.permissions = ECAT_PERM_RW;
    param_desc.max_size = MOCK_PARAM_SIZE;
    param_desc.enabled = true;
    param_desc.driver.io.init = param_init;
    param_desc.driver.io.invalidate = param_invalidate;
    param_desc.driver.io.erase_to = param_erase_to;
    param_desc.driver.io.write = param_write;
    param_desc.driver.io.read = param_read;
    param_desc.driver.io.finalize = param_finalize;
    param_desc.driver.io.abort = param_abort;
    assert(ecat_target_registry_add(&reg, &param_desc) == 0);

    /* 注册 Target 3: 日志区 (READ-ONLY) */
    ecat_target_desc_t log_desc;
    memset(&log_desc, 0, sizeof(log_desc));
    log_desc.target_id = 3U;
    strncpy(log_desc.name, "sys_log", sizeof(log_desc.name));
    log_desc.type = ECAT_TARGET_TYPE_DATA;
    log_desc.permissions = ECAT_PERM_READ;
    log_desc.max_size = 64U * 1024U;
    log_desc.enabled = true;
    log_desc.driver.io.init = log_init;
    log_desc.driver.io.read = log_read;
    log_desc.driver.io.finalize = log_finalize;
    log_desc.driver.io.abort = log_abort;
    log_desc.driver.get_data_size = log_get_size;
    assert(ecat_target_registry_add(&reg, &log_desc) == 0);

    ecat_transfer_t manager;
    assert(ecat_transfer_init(NULL, &reg, NULL) == -1);
    assert(ecat_transfer_init(&manager, NULL, NULL) == -1);
    assert(ecat_transfer_init(&manager, &reg, NULL) == 0);

    const int channel_uart = 10;
    const int channel_can  = 20;

    /* ---------------------------------------------------------------------- */
    /* 1. 测试普通数据导入 (IMPORT): 写入参数区                               */
    /* ---------------------------------------------------------------------- */
    reset_mock_flags();
    uint8_t sample_params[256];
    for (size_t i = 0; i < sizeof(sample_params); i++) {
        sample_params[i] = (uint8_t)(i ^ 0x5A);
    }

    uint64_t token1 = 0;
    ecat_transfer_request_t import_req = {
        .transport_id = 1U, /* UART 通道 */
        .target_id = 2U,    /* 参数区 */
        .direction = ECAT_TRANSFER_DIR_IMPORT,
        .expected_size = sizeof(sample_params)
    };

    assert(ecat_transfer_begin(&manager, &channel_uart, &import_req, &token1) == ECAT_TRANSFER_COMPLETE);
    assert(token1 != 0);
    assert(g_init_called && g_invalidate_called);

    /* 并发互斥验证: 此时 CAN 试图开启会话，应被 BUSY 拦截 */
    uint64_t rejected_token = 0;
    ecat_transfer_request_t conflict_req = {
        .transport_id = 2U,
        .target_id = 3U,
        .direction = ECAT_TRANSFER_DIR_EXPORT
    };
    assert(ecat_transfer_begin(&manager, &channel_can, &conflict_req, &rejected_token) == ECAT_TRANSFER_BUSY);

    /* 分两包写入数据 */
    assert(ecat_transfer_write(&manager, &channel_uart, token1, sample_params, 100) == ECAT_TRANSFER_COMPLETE);
    assert(g_erased_bytes >= 100);
    assert(ecat_transfer_write(&manager, &channel_uart, token1, sample_params + 100, 156) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_get_bytes_transferred(&manager) == 256);

    /* 校验 CRC32 */
    uint32_t expected_crc = ecat_foe_crc32_calc(sample_params, sizeof(sample_params));
    assert(ecat_transfer_get_crc32(&manager) == expected_crc);

    /* 提交会话 */
    assert(ecat_transfer_finish(&manager, &channel_uart, token1) == ECAT_TRANSFER_COMPLETE);
    assert(g_finalize_called);
    assert(memcmp(g_mock_param_flash, sample_params, sizeof(sample_params)) == 0);

    /* 旧令牌已失效 */
    assert(ecat_transfer_write(&manager, &channel_uart, token1, sample_params, 10) == ECAT_TRANSFER_STALE);

    /* ---------------------------------------------------------------------- */
    /* 2. 测试数据导出 (EXPORT): 回读系统日志                                 */
    /* ---------------------------------------------------------------------- */
    reset_mock_flags();
    for (size_t i = 0; i < MOCK_LOG_SIZE; i++) {
        g_mock_log_flash[i] = (uint8_t)((i * 7) & 0xFF);
    }
    uint32_t expected_log_crc = ecat_foe_crc32_calc(g_mock_log_flash, MOCK_LOG_SIZE);

    uint64_t token2 = 0;
    ecat_transfer_request_t export_req = {
        .transport_id = 2U, /* CAN 通道 */
        .target_id = 3U,    /* 日志区 */
        .direction = ECAT_TRANSFER_DIR_EXPORT,
        .expected_size = 0  /* 驱动自动提供尺寸 */
    };

    assert(ecat_transfer_begin(&manager, &channel_can, &export_req, &token2) == ECAT_TRANSFER_COMPLETE);
    assert(g_init_called);
    assert(ecat_transfer_get_total_export_size(&manager) == MOCK_LOG_SIZE);

    /* 模拟按 256 字节块分批读出 */
    uint8_t rx_buffer[MOCK_LOG_SIZE];
    size_t rx_offset = 0;
    while (rx_offset < MOCK_LOG_SIZE) {
        uint8_t chunk[256];
        size_t actual = 0;
        ecat_transfer_result_t res = ecat_transfer_read(
            &manager, &channel_can, token2, chunk, sizeof(chunk), &actual);
        assert(actual > 0);
        memcpy(&rx_buffer[rx_offset], chunk, actual);
        rx_offset += actual;

        if (rx_offset == MOCK_LOG_SIZE) {
            assert(res == ECAT_TRANSFER_COMPLETE);
        } else {
            assert(res == ECAT_TRANSFER_CONTINUE);
        }
    }

    assert(rx_offset == MOCK_LOG_SIZE);
    assert(memcmp(rx_buffer, g_mock_log_flash, MOCK_LOG_SIZE) == 0);
    assert(ecat_transfer_get_crc32(&manager) == expected_log_crc);
    assert(ecat_transfer_finish(&manager, &channel_can, token2) == ECAT_TRANSFER_COMPLETE);
    assert(!g_finalize_called); /* 导出完成不应调用写提交 finalize */

    /* ---------------------------------------------------------------------- */
    /* 3. 权限与非法操作拦截验证                                             */
    /* ---------------------------------------------------------------------- */
    /* 3.1 尝试向仅读日志区写入，应被 DENIED */
    ecat_transfer_request_t write_to_log = {
        .transport_id = 1U,
        .target_id = 3U, /* 日志区 (READ-ONLY) */
        .direction = ECAT_TRANSFER_DIR_IMPORT
    };
    uint64_t dummy_token = 0;
    assert(ecat_transfer_begin(&manager, &channel_uart, &write_to_log, &dummy_token) == ECAT_TRANSFER_DENIED);

    /* 3.2 开启强制 ACL 白名单并测试通道隔离 */
    ecat_target_registry_set_enforce_acl(&reg, true);
    /* 仅允许 UART(1) 访问参数区(2) */
    assert(ecat_target_registry_add_acl(&reg, 1U, 2U, ECAT_PERM_RW) == 0);

    /* CAN(2) 试图读参数区(2)，虽参数区本身支持读，但被 ACL 拦截 */
    ecat_transfer_request_t can_read_param = {
        .transport_id = 2U,
        .target_id = 2U,
        .direction = ECAT_TRANSFER_DIR_EXPORT
    };
    assert(ecat_transfer_begin(&manager, &channel_can, &can_read_param, &dummy_token) == ECAT_TRANSFER_DENIED);

    /* UART(1) 读参数区(2) 应被允许 */
    ecat_transfer_request_t uart_read_param = {
        .transport_id = 1U,
        .target_id = 2U,
        .direction = ECAT_TRANSFER_DIR_EXPORT,
        .expected_size = 64
    };
    assert(ecat_transfer_begin(&manager, &channel_uart, &uart_read_param, &dummy_token) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_cancel(&manager, &channel_uart, dummy_token) == ECAT_TRANSFER_COMPLETE);

    /* ---------------------------------------------------------------------- */
    /* 4. 取消与异常清理 (abort 保证)                                         */
    /* ---------------------------------------------------------------------- */
    reset_mock_flags();
    assert(ecat_transfer_begin(&manager, &channel_uart, &uart_read_param, &dummy_token) == ECAT_TRANSFER_COMPLETE);
    assert(g_init_called);
    assert(!g_abort_called);
    assert(ecat_transfer_cancel(&manager, &channel_uart, dummy_token) == ECAT_TRANSFER_COMPLETE);
    assert(g_abort_called);

    /* ---------------------------------------------------------------------- */
    /* 5. 固件区块核心升级联动测试 (Firmware Target + Core Integration)       */
    /* ---------------------------------------------------------------------- */
    ecat_foe_core_t core;
    ecat_foe_core_cfg_t core_cfg;
    memset(&core_cfg, 0, sizeof(core_cfg));
    core_cfg.project_name = "TEST_PROJ";
    core_cfg.hardware_id = 0x1234U;
    core_cfg.local_hw_ver = 0x0100U;
    core_cfg.max_header_size = 148U;
    core_cfg.targets[0].enabled = true;
    core_cfg.targets[0].max_payload_size = 1024U * 1024U;
    core_cfg.targets[0].write_unit = 8U;
    core_cfg.targets[0].driver = &param_desc.driver.io;
    core_cfg.targets[0].user_ctx = NULL;

    assert(ecat_foe_core_init(&core, &core_cfg) == 0);

    /* 注册 Target 1: 固件区 (FIRMWARE 类型) */
    ecat_target_desc_t fw_desc;
    memset(&fw_desc, 0, sizeof(fw_desc));
    fw_desc.target_id = 1U;
    strncpy(fw_desc.name, "mcu_fw", sizeof(fw_desc.name));
    fw_desc.type = ECAT_TARGET_TYPE_FIRMWARE;
    fw_desc.permissions = ECAT_PERM_WRITE;
    fw_desc.max_size = 1024U * 1024U;
    fw_desc.enabled = true;
    fw_desc.driver.io = param_desc.driver.io;
    assert(ecat_target_registry_add(&reg, &fw_desc) == 0);
    assert(ecat_target_registry_add_acl(&reg, 1U, 1U, ECAT_PERM_WRITE) == 0);

    ecat_transfer_t fw_manager;
    assert(ecat_transfer_init(&fw_manager, &reg, &core) == 0);

    uint64_t fw_token = 0;
    ecat_transfer_request_t fw_req = {
        .transport_id = 1U,
        .target_id = 1U,
        .direction = ECAT_TRANSFER_DIR_IMPORT,
        .expected_size = 1024U,
        .fw_version = 0x01000000U
    };
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &fw_req, &fw_token) == ECAT_TRANSFER_COMPLETE);
    assert(fw_token != 0);
    assert(core.state == ECAT_FOE_STATE_WRQ_ACKED);

    /* 取消固件升级，核实 core 回到 IDLE/ABORT 状态 */
    assert(ecat_transfer_cancel(&fw_manager, &channel_uart, fw_token) == ECAT_TRANSFER_COMPLETE);
    assert(core.state == ECAT_FOE_STATE_IDLE || core.state == ECAT_FOE_STATE_ERROR);

    /* 验证 Issue 2: 只读 EXPORT 生命周期与清理契约 */
    printf("--- 测试 EXPORT 只读生命周期与驱动清理 ---\n");
    reset_mock_flags();
    ecat_transfer_request_t exp_req = {
        .transport_id = 1U,
        .target_id = 2U, /* params (ECAT_TARGET_TYPE_DATA) */
        .direction = ECAT_TRANSFER_DIR_EXPORT,
        .expected_size = 16U,
        .fw_version = 0U
    };
    uint64_t exp_token = 0;
    assert(ecat_transfer_begin(&manager, &channel_uart, &exp_req, &exp_token) == ECAT_TRANSFER_COMPLETE);
    assert(g_init_called == true);
    /* 导出完成，严禁调用 io.finalize (不可用写提交激活镜像) */
    assert(ecat_transfer_finish(&manager, &channel_uart, exp_token) == ECAT_TRANSFER_COMPLETE);
    assert(g_finalize_called == false);

    /* 2.1 固件区块导出取消: 必须清理实际打开的 registry 驱动，且不得误触发 core_abort */
    ecat_target_desc_t fw_export_desc;
    memset(&fw_export_desc, 0, sizeof(fw_export_desc));
    fw_export_desc.target_id = 10U;
    strncpy(fw_export_desc.name, "fw_legacy_drv", sizeof(fw_export_desc.name));
    fw_export_desc.type = ECAT_TARGET_TYPE_FIRMWARE;
    fw_export_desc.permissions = ECAT_PERM_RW;
    fw_export_desc.max_size = 4096U;
    fw_export_desc.enabled = true;
    fw_export_desc.driver.io.init = lifecycle_init;
    fw_export_desc.driver.io.read = lifecycle_read;
    fw_export_desc.driver.io.write = param_write;
    fw_export_desc.driver.io.erase_to = param_erase_to;
    fw_export_desc.driver.io.finalize = lifecycle_finalize;
    fw_export_desc.driver.io.abort = lifecycle_abort;
    assert(ecat_target_registry_add(&reg, &fw_export_desc) == 0);
    assert(ecat_target_registry_add_acl(&reg, 0U, 10U, ECAT_PERM_RW) == 0);

    reset_mock_flags();
    exp_req.target_id = 10U;
    exp_req.direction = ECAT_TRANSFER_DIR_EXPORT;
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &exp_req, &exp_token) == ECAT_TRANSFER_COMPLETE);
    assert(g_init_called == true);
    assert(g_driver_locked == 1);
    assert(core.state == ECAT_FOE_STATE_IDLE);
    /* 取消导出: 必须清理注册表驱动 (locked 归零并调用 abort)，且 core 保持 IDLE */
    assert(ecat_transfer_cancel(&fw_manager, &channel_uart, exp_token) == ECAT_TRANSFER_COMPLETE);
    assert(g_abort_called == true);
    assert(g_driver_locked == 0);
    assert(core.state == ECAT_FOE_STATE_IDLE);

    /* 2.2 专用只读生命周期 (read_open / read_close): 不触碰写提交与写失效 */
    ecat_target_desc_t ro_desc;
    memset(&ro_desc, 0, sizeof(ro_desc));
    ro_desc.target_id = 11U;
    strncpy(ro_desc.name, "ro_lifecycle", sizeof(ro_desc.name));
    ro_desc.type = ECAT_TARGET_TYPE_FIRMWARE;
    ro_desc.permissions = ECAT_PERM_RW;
    ro_desc.max_size = 4096U;
    ro_desc.enabled = true;
    ro_desc.driver.read_open = lifecycle_read_open;
    ro_desc.driver.read_close = lifecycle_read_close;
    ro_desc.driver.io.init = lifecycle_init;
    ro_desc.driver.io.read = lifecycle_read;
    ro_desc.driver.io.write = param_write;
    ro_desc.driver.io.erase_to = param_erase_to;
    ro_desc.driver.io.finalize = lifecycle_finalize;
    ro_desc.driver.io.abort = lifecycle_abort;
    assert(ecat_target_registry_add(&reg, &ro_desc) == 0);
    assert(ecat_target_registry_add_acl(&reg, 0U, 11U, ECAT_PERM_RW) == 0);

    /* 导出成功生命周期: 保持现有 valid 标记不变 */
    reset_mock_flags();
    g_valid_state = 1; /* 介质中已有合法镜像 */
    exp_req.target_id = 11U;
    assert(ecat_transfer_begin(&manager, &channel_uart, &exp_req, &exp_token) == ECAT_TRANSFER_COMPLETE);
    assert(g_read_open_called == true);
    assert(g_init_called == false); /* read_open 优先于 io.init */
    assert(g_driver_locked == 1);
    assert(ecat_transfer_finish(&manager, &channel_uart, exp_token) == ECAT_TRANSFER_COMPLETE);
    assert(g_read_close_called == true);
    assert(g_driver_locked == 0);
    assert(g_finalize_called == false);
    assert(g_valid_state == 1); /* 有效标记未被篡改 */

    /* 导出取消生命周期: 不可在读取消时使镜像失效 */
    reset_mock_flags();
    g_valid_state = 1;
    assert(ecat_transfer_begin(&manager, &channel_uart, &exp_req, &exp_token) == ECAT_TRANSFER_COMPLETE);
    assert(g_read_open_called == true);
    assert(g_driver_locked == 1);
    assert(ecat_transfer_cancel(&manager, &channel_uart, exp_token) == ECAT_TRANSFER_COMPLETE);
    assert(g_read_close_called == true);
    assert(g_driver_locked == 0);
    assert(g_abort_called == false);
    assert(g_valid_state == 1); /* 取消读取绝不使原有固件镜像失效 */

    /* 2.3 读失败错误处理与只读会话清理 */
    reset_mock_flags();
    assert(ecat_transfer_begin(&manager, &channel_uart, &exp_req, &exp_token) == ECAT_TRANSFER_COMPLETE);
    assert(g_driver_locked == 1);
    g_fail_read = true;
    uint8_t read_chunk[16];
    size_t actual_r = 0;
    assert(ecat_transfer_read(&manager, &channel_uart, exp_token, read_chunk, sizeof(read_chunk), &actual_r) == ECAT_TRANSFER_FAILED);
    assert(g_read_close_called == true);
    assert(g_driver_locked == 0);
    /* 会话已释放，其他 owner 可以立即开始 */
    g_fail_read = false;
    uint64_t next_token = 0;
    assert(ecat_transfer_begin(&manager, &channel_can, &exp_req, &next_token) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_finish(&manager, &channel_can, next_token) == ECAT_TRANSFER_COMPLETE);

    /* 2.4 初始化失败不污染驱动状态与会话 */
    reset_mock_flags();
    g_fail_init = true;
    assert(ecat_transfer_begin(&manager, &channel_uart, &exp_req, &exp_token) == ECAT_TRANSFER_FAILED);
    assert(g_driver_locked == 0);
    g_fail_init = false;
    assert(ecat_transfer_begin(&manager, &channel_uart, &exp_req, &exp_token) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_finish(&manager, &channel_uart, exp_token) == ECAT_TRANSFER_COMPLETE);

    /* 验证 Issue 3: 严格校验固件注册表容量与持续写入约束 */
    printf("--- 测试 Issue 3: 固件容量约束与注册表/核心对齐 ---\n");
    static uint8_t s_oversize_efw[ECAT_FOE_EFW_HEADER_MIN_LEN + 257U];
    static uint8_t s_valid_16_efw[ECAT_FOE_EFW_HEADER_MIN_LEN + 16U];
    {
        uint32_t plen = 257U;
        uint8_t *out = s_oversize_efw;
        memset(out, 0, sizeof(s_oversize_efw));
        out[0] = 0x45; out[1] = 0x43; out[2] = 0x46; out[3] = 0x57;
        out[4] = ECAT_FOE_EFW_FORMAT_VERSION;
        out[6] = (uint8_t)ECAT_FOE_EFW_HEADER_MIN_LEN;
        out[7] = (uint8_t)(ECAT_FOE_EFW_HEADER_MIN_LEN >> 8);
        out[8] = 0x01; out[9] = 0x00; out[10] = 0x01; out[11] = 0x10; /* project_id = 0x10010001 */
        out[12] = 0x01; /* hardware_id = 1 */
        out[16] = 1; out[18] = 1; out[20] = 1; /* target_id = 1 */
        out[24] = 4; out[25] = 3; out[26] = 2; out[27] = 1;
        out[28] = 0; out[29] = 0; out[30] = 0; out[31] = 1;
        out[32] = (uint8_t)plen; out[33] = (uint8_t)(plen >> 8);
        for (uint32_t i = 0; i < plen; ++i) out[ECAT_FOE_EFW_HEADER_MIN_LEN + i] = (uint8_t)(i & 0xFF);
        uint32_t pcrc = ecat_foe_crc32_calc(out + ECAT_FOE_EFW_HEADER_MIN_LEN, plen);
        out[36] = (uint8_t)pcrc; out[37] = (uint8_t)(pcrc >> 8);
        out[38] = (uint8_t)(pcrc >> 16); out[39] = (uint8_t)(pcrc >> 24);
        memcpy(out + 60U, "SIG2", 4U);
        out[64] = 1U;
        uint32_t hcrc = ecat_foe_crc32_calc(out, ECAT_FOE_EFW_HEADER_MIN_LEN - 4U);
        out[144] = (uint8_t)hcrc; out[145] = (uint8_t)(hcrc >> 8);
        out[146] = (uint8_t)(hcrc >> 16); out[147] = (uint8_t)(hcrc >> 24);
    }
    {
        uint32_t plen = 16U;
        uint8_t *out = s_valid_16_efw;
        memset(out, 0, sizeof(s_valid_16_efw));
        out[0] = 0x45; out[1] = 0x43; out[2] = 0x46; out[3] = 0x57;
        out[4] = ECAT_FOE_EFW_FORMAT_VERSION;
        out[6] = (uint8_t)ECAT_FOE_EFW_HEADER_MIN_LEN;
        out[7] = (uint8_t)(ECAT_FOE_EFW_HEADER_MIN_LEN >> 8);
        out[8] = 0x01; out[9] = 0x00; out[10] = 0x01; out[11] = 0x10;
        out[12] = 0x01;
        out[16] = 1; out[18] = 1; out[20] = 1;
        out[24] = 4; out[25] = 3; out[26] = 2; out[27] = 1;
        out[28] = 0; out[29] = 0; out[30] = 0; out[31] = 1;
        out[32] = (uint8_t)plen; out[33] = (uint8_t)(plen >> 8);
        for (uint32_t i = 0; i < plen; ++i) out[ECAT_FOE_EFW_HEADER_MIN_LEN + i] = (uint8_t)(i & 0xFF);
        uint32_t pcrc = ecat_foe_crc32_calc(out + ECAT_FOE_EFW_HEADER_MIN_LEN, plen);
        out[36] = (uint8_t)pcrc; out[37] = (uint8_t)(pcrc >> 8);
        out[38] = (uint8_t)(pcrc >> 16); out[39] = (uint8_t)(pcrc >> 24);
        memcpy(out + 60U, "SIG2", 4U);
        out[64] = 1U;
        uint32_t hcrc = ecat_foe_crc32_calc(out, ECAT_FOE_EFW_HEADER_MIN_LEN - 4U);
        out[144] = (uint8_t)hcrc; out[145] = (uint8_t)(hcrc >> 8);
        out[146] = (uint8_t)(hcrc >> 16); out[147] = (uint8_t)(hcrc >> 24);
    }

    /* 将 Target 1 (mcu_fw) 在注册表中的容量设为 16 字节，而 core.cfg 中为 1024 字节 */
    const ecat_target_desc_t *fw_reg_desc = ecat_target_registry_find_by_id(&reg, 1U);
    assert(fw_reg_desc != NULL);
    ((ecat_target_desc_t *)fw_reg_desc)->max_size = 16U;
    core.cfg.project_id = 0x10010001U;
    core.cfg.hardware_id = 1U;
    core.cfg.local_hw_ver = 1U;
    core.cfg.targets[0].max_payload_size = 1024U;

    /* 3.1 未知长度下超限有效负载拦截 */
    ecat_transfer_request_t oversize_req = {
        .transport_id = 1U,
        .target_id = 1U,
        .direction = ECAT_TRANSFER_DIR_IMPORT,
        .expected_size = 0U, /* 未知总长 */
        .fw_version = 0x01020304U
    };
    uint64_t over_token = 0;
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &oversize_req, &over_token) == ECAT_TRANSFER_COMPLETE);
    int write_rc = ecat_transfer_write(&fw_manager, &channel_uart, over_token, s_oversize_efw, sizeof(s_oversize_efw));
    /* 257 字节 payload 超过注册表 max_size=16，必须写入失败且核心未写入任何介质 */
    assert(write_rc != ECAT_TRANSFER_COMPLETE);
    assert(core.written_offset == 0);

    /* 3.2 分片写入场景下的超限拦截 */
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &oversize_req, &over_token) == ECAT_TRANSFER_COMPLETE);
    size_t chunk_sz = 32U;
    bool chunk_failed = false;
    for (size_t off = 0; off < sizeof(s_oversize_efw); off += chunk_sz) {
        size_t this_sz = (sizeof(s_oversize_efw) - off < chunk_sz) ? (sizeof(s_oversize_efw) - off) : chunk_sz;
        if (ecat_transfer_write(&fw_manager, &channel_uart, over_token, &s_oversize_efw[off], this_sz) != ECAT_TRANSFER_COMPLETE) {
            chunk_failed = true;
            break;
        }
    }
    assert(chunk_failed == true);
    assert(core.written_offset == 0);

    /* 3.3 声明总长但传输截断检测 */
    ecat_transfer_request_t trunc_req = {
        .transport_id = 1U,
        .target_id = 1U,
        .direction = ECAT_TRANSFER_DIR_IMPORT,
        .expected_size = sizeof(s_valid_16_efw),
        .fw_version = 0x01020304U
    };
    uint64_t trunc_token = 0;
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &trunc_req, &trunc_token) == ECAT_TRANSFER_COMPLETE);
    /* 仅发送头部，未发送 payload 即尝试 finish */
    assert(ecat_transfer_write(&fw_manager, &channel_uart, trunc_token, s_valid_16_efw, ECAT_FOE_EFW_HEADER_MIN_LEN) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_finish(&fw_manager, &channel_uart, trunc_token) == ECAT_TRANSFER_FAILED);

    /* 3.4 声明总长超出物理上限在 begin 时即被拦截 */
    ecat_transfer_request_t too_large_req = {
        .transport_id = 1U,
        .target_id = 1U,
        .direction = ECAT_TRANSFER_DIR_IMPORT,
        .expected_size = 16U + 148U + 8U + 10U, /* 超出 max_size(16) + 148 + 7 */
        .fw_version = 0x01020304U
    };
    uint64_t dummy_t = 0;
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &too_large_req, &dummy_t) == ECAT_TRANSFER_FAILED);

    /* 3.5 合法 16 字节有效负载端到端导入成功 */
    uint64_t valid_token = 0;
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &trunc_req, &valid_token) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_write(&fw_manager, &channel_uart, valid_token, s_valid_16_efw, sizeof(s_valid_16_efw)) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_finish(&fw_manager, &channel_uart, valid_token) == ECAT_TRANSFER_COMPLETE);
    assert(core.written_offset == 16U);

    /* 验证 Issue 4: 统一旧升级入口 (ecat_upgrade_t) 与新传输管理器的会话仲裁与 ACL */
    printf("--- 测试 Issue 4: 统一旧升级入口与新管理器的会话仲裁与 ACL ---\n");
    ecat_foe_core_on_abort(&core);
    ecat_upgrade_t old_mgr;
    assert(ecat_upgrade_init(&old_mgr, &core) == 0);
    ecat_upgrade_request_t old_req = {
        .target_id = 1U,
        .fw_version = 0x01020304U
    };
    uint64_t old_token = 0;
    assert(ecat_upgrade_begin(&old_mgr, &channel_can, &old_req, &old_token) == ECAT_UPGRADE_COMPLETE);
    assert(old_token != 0);

    /* 4.1 旧入口活动期间，新管理器对共享 core 发起请求必须返回 BUSY */
    uint64_t new_token = 0;
    ecat_transfer_request_t new_req = {
        .transport_id = 1U,
        .target_id = 1U,
        .direction = ECAT_TRANSFER_DIR_IMPORT,
        .expected_size = sizeof(s_valid_16_efw),
        .fw_version = 0x01020304U
    };
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &new_req, &new_token) == ECAT_TRANSFER_BUSY);
    /* 新管理器无法取消旧入口会话 */
    assert(ecat_transfer_cancel(&fw_manager, &channel_uart, old_token) == ECAT_TRANSFER_STALE);
    assert(ecat_upgrade_session_owns(old_mgr.session_ptr, &channel_can, old_token));

    /* 释放旧入口会话 */
    assert(ecat_upgrade_cancel(&old_mgr, &channel_can, old_token) == ECAT_UPGRADE_COMPLETE);

    /* 4.2 反向仲裁: 新管理器活动期间，旧入口发起请求必须返回 BUSY */
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &new_req, &new_token) == ECAT_TRANSFER_COMPLETE);
    assert(new_token != 0);
    uint64_t dummy_old_token = 0;
    assert(ecat_upgrade_begin(&old_mgr, &channel_can, &old_req, &dummy_old_token) == ECAT_UPGRADE_BUSY);
    /* 旧入口无法取消新管理器持有的会话 */
    assert(ecat_upgrade_cancel(&old_mgr, &channel_can, new_token) == ECAT_UPGRADE_STALE);
    assert(ecat_transfer_cancel(&fw_manager, &channel_uart, new_token) == ECAT_TRANSFER_COMPLETE);

    /* 4.3 真实 FoE 传输适配器 (ecat_foe_transport_t) 仲裁竞争测试 */
    ecat_foe_transport_t foe_tr;
    assert(ecat_foe_transport_init(&foe_tr, &old_mgr) == 0);
    const char *wrq_name = "TEST_PROJ_MCU_V1.2.3.4.efw";
    size_t wrq_name_len = strlen(wrq_name);

    /* 新管理器占有时，FoE 适配器 begin 返回 wire error (ECAT_FOE_ERRCODE_NOTDEFINED) */
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &new_req, &new_token) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_foe_transport_begin(&foe_tr, wrq_name, wrq_name_len) == ECAT_FOE_ERRCODE_NOTDEFINED);
    assert(ecat_transfer_cancel(&fw_manager, &channel_uart, new_token) == ECAT_TRANSFER_COMPLETE);

    /* FoE 适配器激活时，新管理器 begin 返回 BUSY */
    assert(ecat_foe_transport_begin(&foe_tr, wrq_name, wrq_name_len) == 0);
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &new_req, &new_token) == ECAT_TRANSFER_BUSY);
    ecat_foe_transport_abort(&foe_tr);

    /* 4.4 真实文件传输适配器 (ecat_upgrade_file_t) 仲裁竞争测试 */
    uint8_t file_buf[128];
    ecat_upgrade_file_t file_tr;
    assert(ecat_upgrade_file_init(&file_tr, &old_mgr, file_buf, sizeof(file_buf)) == 0);
    FILE *dummy_file = tmpfile();
    assert(dummy_file != NULL);

    /* 新管理器占有时，File 适配器 begin 返回 BUSY */
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &new_req, &new_token) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_upgrade_file_begin(&file_tr, dummy_file, &old_req) == ECAT_UPGRADE_BUSY);
    assert(ecat_transfer_cancel(&fw_manager, &channel_uart, new_token) == ECAT_TRANSFER_COMPLETE);

    /* File 适配器激活时，新管理器 begin 返回 BUSY */
    assert(ecat_upgrade_file_begin(&file_tr, dummy_file, &old_req) == ECAT_UPGRADE_COMPLETE);
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &new_req, &new_token) == ECAT_TRANSFER_BUSY);
    assert(ecat_upgrade_file_cancel(&file_tr) == ECAT_UPGRADE_COMPLETE);
    fclose(dummy_file);

    /* 4.5 统一 ACL 管控: 旧入口及适配器不能绕过注册表权限管控 */
    size_t saved_rule_cnt = reg.acl_rule_count;
    reg.acl_rule_count = 0;
    ecat_target_registry_set_enforce_acl(&reg, true);
    /* 此时未向 FoE (transport_id=1) 配置 Target 1 的写权限，旧入口必须被拦截 */
    assert(ecat_upgrade_begin(&old_mgr, &channel_can, &old_req, &old_token) == ECAT_UPGRADE_FAILED);
    assert(old_mgr.last_error == ECAT_FOE_CORE_ERR_WRQ_TARGET_DISABLED);
    assert(ecat_foe_transport_begin(&foe_tr, wrq_name, wrq_name_len) == ECAT_FOE_ERRCODE_NOTDEFINED);

    /* 恢复 ACL 白名单授权 FoE (ID=1) 对 Target 1 写入 */
    reg.acl_rule_count = saved_rule_cnt;
    assert(ecat_upgrade_begin(&old_mgr, &channel_can, &old_req, &old_token) == ECAT_UPGRADE_COMPLETE);
    assert(ecat_upgrade_cancel(&old_mgr, &channel_can, old_token) == ECAT_UPGRADE_COMPLETE);
    ecat_target_registry_set_enforce_acl(&reg, false);

    /* ---------------------------------------------------------------------- */
    /* 6. Issue 6: 传输方向严格枚举校验与防驱动副作用/占锁测试              */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试 Issue 6: 非法传输方向严格枚举校验与防驱动副作用 ---\n");
    ecat_target_registry_set_enforce_acl(&reg, true);
    /* 针对 Target 2 仅授予 READ-only 权限 */
    reg.acl_rule_count = 0;
    assert(ecat_target_registry_add_acl(&reg, 1U, 2U, ECAT_PERM_READ) == 0);

    reset_mock_flags();
    ecat_transfer_request_t invalid_dir_req = {
        .transport_id = 1U,
        .target_id = 2U,
        .direction = (ecat_transfer_dir_t)99, /* 非法枚举值 */
        .expected_size = 16U,
        .fw_version = 0U
    };
    uint64_t invalid_dir_token = 0;
    ecat_transfer_result_t bad_dir_res = ecat_transfer_begin(&fw_manager, &channel_uart, &invalid_dir_req, &invalid_dir_token);
    /* 断言 1: 非法方向必须立即被拒绝返回 ECAT_TRANSFER_INVALID */
    assert(bad_dir_res == ECAT_TRANSFER_INVALID);
    /* 断言 2: 未调用底层驱动任何生命周期函数 (禁止 invalidate 或 init) */
    assert(g_invalidate_called == false);
    assert(g_init_called == false);
    /* 断言 3: 未占用会话锁，token 为 0，且后续合法请求可正常发起 */
    assert(invalid_dir_token == 0);
    assert(fw_manager.session.owner == NULL);
    ecat_target_registry_set_enforce_acl(&reg, false);

    /* ---------------------------------------------------------------------- */
    /* 7. Issue 7: DATA 导入声明 expected_size 严格执行 (少收/多收/恰好/未知) */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试 Issue 7: DATA 导入声明长度约束与完成校验 ---\n");
    uint8_t data_bytes[64] = {0x55};

    /* 7.1 少收测试: 声明 expected=16，写 4 字节，finish 必须失败并中止驱动 */
    reset_mock_flags();
    ecat_transfer_request_t req_data_16 = {
        .transport_id = 1U,
        .target_id = 2U, /* Target 2 是 DATA 类型 */
        .direction = ECAT_TRANSFER_DIR_IMPORT,
        .expected_size = 16U,
        .fw_version = 0U
    };
    uint64_t data_tok = 0;
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &req_data_16, &data_tok) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_write(&fw_manager, &channel_uart, data_tok, data_bytes, 4U) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_finish(&fw_manager, &channel_uart, data_tok) == ECAT_TRANSFER_FAILED);
    assert(fw_manager.last_error == ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED);
    assert(g_abort_called == true);
    assert(fw_manager.session.owner == NULL);

    /* 7.2 多收测试: 声明 expected=16，尝试写 32 字节，write 必须失败并中止驱动 */
    reset_mock_flags();
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &req_data_16, &data_tok) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_write(&fw_manager, &channel_uart, data_tok, data_bytes, 32U) == ECAT_TRANSFER_FAILED);
    assert(fw_manager.last_error == ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED);
    assert(g_abort_called == true);
    assert(fw_manager.session.owner == NULL);

    /* 7.3 恰好测试: 声明 expected=16，写入恰好 16 字节，finish 必须成功并 finalize */
    reset_mock_flags();
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &req_data_16, &data_tok) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_write(&fw_manager, &channel_uart, data_tok, data_bytes, 16U) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_finish(&fw_manager, &channel_uart, data_tok) == ECAT_TRANSFER_COMPLETE);
    assert(g_finalize_called == true);
    assert(fw_manager.session.owner == NULL);

    /* 7.4 未知长度测试: 声明 expected=0，流式写入 24 字节，finish 必须成功并 finalize */
    reset_mock_flags();
    ecat_transfer_request_t req_data_unknown = {
        .transport_id = 1U,
        .target_id = 2U,
        .direction = ECAT_TRANSFER_DIR_IMPORT,
        .expected_size = 0U,
        .fw_version = 0U
    };
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &req_data_unknown, &data_tok) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_write(&fw_manager, &channel_uart, data_tok, data_bytes, 24U) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_finish(&fw_manager, &channel_uart, data_tok) == ECAT_TRANSFER_COMPLETE);
    assert(g_finalize_called == true);
    assert(fw_manager.session.owner == NULL);

    /* ---------------------------------------------------------------------- */
    /* 8. Issue 8: 64位超大尺寸 (UINT32_MAX+2) 整数窄化与缓冲区溢出防护     */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试 Issue 8: 64 位大尺寸长度防窄化与安全拦截 ---\n");
#if __SIZEOF_SIZE_T__ > 4
    reset_mock_flags();
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &req_data_unknown, &data_tok) == ECAT_TRANSFER_COMPLETE);
    uint8_t single_byte = 0xAA;
    /* 传入 64 位大长度 UINT32_MAX + 2，若窄化为 1 字节将越界访问 single_byte 后续内存 */
    ecat_transfer_result_t oversize_res = ecat_transfer_write(
        &fw_manager, &channel_uart, data_tok, &single_byte, (size_t)UINT32_MAX + 2U);
    assert(oversize_res == ECAT_TRANSFER_FAILED);
    assert(fw_manager.last_error == ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED);
    assert(g_abort_called == true);
    assert(fw_manager.session.owner == NULL);

    /* 导出路径防窄化测试: 传入超大容量缓冲，必须正确计算剩余量而非窄化截断 */
    ecat_transfer_request_t exp_req_oversize = {
        .transport_id = 1U,
        .target_id = 2U,
        .direction = ECAT_TRANSFER_DIR_EXPORT,
        .expected_size = 16U,
        .fw_version = 0U
    };
    uint64_t exp_tok = 0;
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &exp_req_oversize, &exp_tok) == ECAT_TRANSFER_COMPLETE);
    uint8_t exp_out[32];
    size_t exp_actual = 0;
    assert(ecat_transfer_read(&fw_manager, &channel_uart, exp_tok, exp_out, (size_t)UINT32_MAX + 2U, &exp_actual) == ECAT_TRANSFER_COMPLETE);
    assert(exp_actual == 16U);
    assert(ecat_transfer_finish(&fw_manager, &channel_uart, exp_tok) == ECAT_TRANSFER_COMPLETE);
#endif

    printf(">>> 双向数据传输引擎全功能单元测试全部通过！<<<\n");
    return 0;
}
