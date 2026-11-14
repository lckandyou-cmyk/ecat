/**
 * @file test_transfer_bidirectional.c
 * @brief 双向数据传输 (导入与导出) 引擎全功能单元测试
 */

#include "ecat_transfer.h"
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

    printf(">>> 双向数据传输引擎全功能单元测试全部通过！<<<\n");
    return 0;
}
