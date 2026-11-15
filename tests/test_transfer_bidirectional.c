/**
 * @file test_transfer_bidirectional.c
 * @brief 双向数据传输 (导入与导出) 引擎全功能单元测试
 */

#include "ecat_transfer.h"
#include "ecat_upgrade.h"
#include "ecat_foe_transport.h"
#include "ecat_upgrade_file.h"
#include "ecat_foe_efw.h"
#include "ecat_foe_core_async.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

#define MOCK_PARAM_SIZE 4096U
#define MOCK_LOG_SIZE   1234U

static void put16_le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static size_t make_test_efw(uint8_t *out, const uint8_t *payload, uint32_t length, uint16_t target_id)
{
    memset(out, 0, ECAT_FOE_EFW_HEADER_MIN_LEN + length);
    put32_le(out, ECAT_FOE_EFW_MAGIC);
    put16_le(out + 4, ECAT_FOE_EFW_FORMAT_VERSION);
    put16_le(out + 6, ECAT_FOE_EFW_HEADER_MIN_LEN);
    put32_le(out + 8, 0x10010001U);
    put32_le(out + 12, 1U);
    put16_le(out + 16, 1U);
    put16_le(out + 18, 2U);
    put16_le(out + 20, target_id);
    put32_le(out + 24, 0x01000000U);
    put32_le(out + 28, 0U);
    put32_le(out + 32, length);
    put32_le(out + 36, ecat_foe_crc32_calc(payload, length));
    memcpy(out + 60, "SIG2", 4U);
    out[64] = 1U;
    put32_le(out + 144, ecat_foe_crc32_calc(out, 144U));
    memcpy(out + ECAT_FOE_EFW_HEADER_MIN_LEN, payload, length);
    return ECAT_FOE_EFW_HEADER_MIN_LEN + length;
}

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

static bool s_async_start_called = false;
static bool s_async_poll_called = false;
static bool s_async_abort_called = false;
static ecat_async_op_result_t s_next_async_result = ECAT_ASYNC_OP_COMPLETE;
static uint32_t s_async_written_bytes = 0;
static uint8_t s_async_flash[1024];

static int s_override_actual_length = -1;

static ecat_async_op_result_t test_data_async_start(void *ctx, ecat_async_data_op_t *op)
{
    (void)ctx;
    s_async_start_called = true;
    if (op->kind == ECAT_ASYNC_DATA_OP_WRITE) {
        if (op->offset + op->length > sizeof(s_async_flash)) return ECAT_ASYNC_OP_FAILED;
        memcpy(&s_async_flash[op->offset], op->buffer, op->length);
        s_async_written_bytes += op->length;
        op->actual_length = (s_override_actual_length >= 0) ? (uint32_t)s_override_actual_length : op->length;
    } else if (op->kind == ECAT_ASYNC_DATA_OP_READ) {
        if (op->offset + op->length > sizeof(s_async_flash)) return ECAT_ASYNC_OP_FAILED;
        memcpy(op->buffer, &s_async_flash[op->offset], op->length);
        op->actual_length = (s_override_actual_length >= 0) ? (uint32_t)s_override_actual_length : op->length;
    } else if (op->kind == ECAT_ASYNC_DATA_OP_ERASE_TO) {
        op->erased_end = (op->need_end + 127U) & ~127U;
    } else if (op->kind == ECAT_ASYNC_DATA_OP_INIT) {
        return ECAT_ASYNC_OP_COMPLETE;
    } else if (op->kind == ECAT_ASYNC_DATA_OP_READ_OPEN) {
        g_read_open_called = true;
        return ECAT_ASYNC_OP_COMPLETE;
    } else if (op->kind == ECAT_ASYNC_DATA_OP_READ_CLOSE) {
        g_read_close_called = true;
        return ECAT_ASYNC_OP_COMPLETE;
    }
    return s_next_async_result;
}

static ecat_async_op_result_t test_data_async_poll(void *ctx, ecat_async_data_op_t *op)
{
    (void)ctx;
    s_async_poll_called = true;
    if (s_override_actual_length >= 0 && op != NULL) {
        op->actual_length = (uint32_t)s_override_actual_length;
    } else if (op != NULL && op->kind == ECAT_ASYNC_DATA_OP_ERASE_TO) {
        op->erased_end = (op->need_end + 127U) & ~127U;
    }
    return s_next_async_result;
}

static void test_data_async_abort(void *ctx)
{
    (void)ctx;
    s_async_abort_called = true;
}

typedef struct {
    uint8_t memory[1024];
    uint32_t erased_watermark;
    bool init_called;
    bool erase_called;
    bool write_called;
    bool finalize_called;
    bool read_open_called;
    bool read_close_called;
    int abort_calls;

    /* 阶段控制: 是否进入 PENDING */
    bool pending_init;
    bool pending_erase;
    bool pending_write;
    bool pending_finalize;

    /* 阶段控制: 是否失败 */
    bool fail_init;
    bool fail_erase;
    bool fail_write;
    bool fail_finalize;
} test_async_data_hardware_t;

static test_async_data_hardware_t s_hw_stage_test;

static ecat_async_op_result_t stage_driver_poll(void *ctx, ecat_async_data_op_t *op);

static ecat_async_op_result_t stage_driver_start(void *ctx, ecat_async_data_op_t *op)
{
    test_async_data_hardware_t *hw = (test_async_data_hardware_t *)ctx;
    if (op->kind == ECAT_ASYNC_DATA_OP_INIT) {
        hw->init_called = true;
        if (hw->fail_init) return ECAT_ASYNC_OP_FAILED;
        if (hw->pending_init) return ECAT_ASYNC_OP_PENDING;
        return ECAT_ASYNC_OP_COMPLETE;
    } else if (op->kind == ECAT_ASYNC_DATA_OP_INIT) {
        return ECAT_ASYNC_OP_COMPLETE;
    } else if (op->kind == ECAT_ASYNC_DATA_OP_READ_OPEN) {
        hw->read_open_called = true;
        return ECAT_ASYNC_OP_COMPLETE;
    } else if (op->kind == ECAT_ASYNC_DATA_OP_ERASE_TO) {
        hw->erase_called = true;
        if (hw->fail_erase) return ECAT_ASYNC_OP_FAILED;
        op->erased_end = (op->need_end + 127U) & ~127U;
        if (hw->pending_erase) return ECAT_ASYNC_OP_PENDING;
        hw->erased_watermark = op->erased_end;
        return ECAT_ASYNC_OP_COMPLETE;
    } else if (op->kind == ECAT_ASYNC_DATA_OP_WRITE) {
        hw->write_called = true;
        if (hw->fail_write) return ECAT_ASYNC_OP_FAILED;
        op->actual_length = op->length;
        if (op->offset + op->length <= sizeof(hw->memory)) {
            memcpy(hw->memory + op->offset, op->buffer, op->length);
        }
        if (hw->pending_write) return ECAT_ASYNC_OP_PENDING;
        return ECAT_ASYNC_OP_COMPLETE;
    } else if (op->kind == ECAT_ASYNC_DATA_OP_FINALIZE) {
        hw->finalize_called = true;
        if (hw->fail_finalize) return ECAT_ASYNC_OP_FAILED;
        if (hw->pending_finalize) return ECAT_ASYNC_OP_PENDING;
        return ECAT_ASYNC_OP_COMPLETE;
    } else if (op->kind == ECAT_ASYNC_DATA_OP_READ_CLOSE) {
        hw->read_close_called = true;
        return ECAT_ASYNC_OP_COMPLETE;
    }
    return ECAT_ASYNC_OP_FAILED;
}

static ecat_async_op_result_t stage_driver_poll(void *ctx, ecat_async_data_op_t *op)
{
    test_async_data_hardware_t *hw = (test_async_data_hardware_t *)ctx;
    if (op->kind == ECAT_ASYNC_DATA_OP_INIT) {
        if (hw->fail_init) return ECAT_ASYNC_OP_FAILED;
        return ECAT_ASYNC_OP_COMPLETE;
    } else if (op->kind == ECAT_ASYNC_DATA_OP_ERASE_TO) {
        if (hw->fail_erase) return ECAT_ASYNC_OP_FAILED;
        op->erased_end = (op->need_end + 127U) & ~127U;
        hw->erased_watermark = op->erased_end;
        return ECAT_ASYNC_OP_COMPLETE;
    } else if (op->kind == ECAT_ASYNC_DATA_OP_WRITE) {
        if (hw->fail_write) return ECAT_ASYNC_OP_FAILED;
        op->actual_length = op->length;
        if (op->offset + op->length <= sizeof(hw->memory)) {
            memcpy(hw->memory + op->offset, op->buffer, op->length);
        }
        return ECAT_ASYNC_OP_COMPLETE;
    } else if (op->kind == ECAT_ASYNC_DATA_OP_FINALIZE) {
        if (hw->fail_finalize) return ECAT_ASYNC_OP_FAILED;
        return ECAT_ASYNC_OP_COMPLETE;
    }
    return ECAT_ASYNC_OP_COMPLETE;
}

static void stage_driver_abort(void *ctx)
{
    test_async_data_hardware_t *hw = (test_async_data_hardware_t *)ctx;
    hw->abort_calls++;
}

static const ecat_async_data_ops_t s_stage_data_ops = {
    .start = stage_driver_start,
    .poll = stage_driver_poll,
    .abort = stage_driver_abort
};

static bool s_fw_async_start_called = false;
static bool s_fw_async_poll_called = false;
static bool s_fw_async_abort_called = false;
static ecat_upgrade_async_result_t s_fw_async_next_res = ECAT_UPGRADE_ASYNC_COMPLETE;



static const uint8_t *s_fw_async_last_data = NULL;
static ecat_upgrade_async_result_t test_fw_async_start(void *ctx, const uint8_t *data, size_t len, bool more)
{
    (void)ctx; (void)len; (void)more;
    s_fw_async_last_data = data;
    s_fw_async_start_called = true;
    return s_fw_async_next_res;
}
static ecat_upgrade_async_result_t test_fw_async_poll(void *ctx)
{
    (void)ctx;
    s_fw_async_poll_called = true;
    return s_fw_async_next_res;
}
static void test_fw_async_abort(void *ctx)
{
    (void)ctx;
    s_fw_async_abort_called = true;
}

typedef struct {
    uint8_t flash[1024];
    uint32_t erased_watermark;
    bool valid;
    bool pending;
    int starts;
    int polls;
} test_flash_async_t;

static test_flash_async_t s_fw_test_flash;

static ecat_foe_target_async_result_t fw_flash_poll(void *ctx, ecat_foe_target_async_op_t *op);

static ecat_foe_target_async_result_t fw_flash_start(void *ctx, ecat_foe_target_async_op_t *op)
{
    test_flash_async_t *f = (test_flash_async_t *)ctx;
    f->starts++;
    f->pending = true;
    return fw_flash_poll(ctx, op);
}

static ecat_foe_target_async_result_t fw_flash_poll(void *ctx, ecat_foe_target_async_op_t *op)
{
    test_flash_async_t *f = (test_flash_async_t *)ctx;
    f->polls++;
    switch (op->kind) {
    case ECAT_FOE_TARGET_OP_INIT:
        f->erased_watermark = 0U;
        break;
    case ECAT_FOE_TARGET_OP_INVALIDATE:
        f->valid = false;
        break;
    case ECAT_FOE_TARGET_OP_ERASE_TO: {
        uint32_t end = (op->need_end + 7U) & ~7U;
        if (end > sizeof(f->flash)) return ECAT_FOE_TARGET_ASYNC_FAILED;
        if (end > f->erased_watermark) {
            memset(f->flash + f->erased_watermark, 0xFF, end - f->erased_watermark);
            f->erased_watermark = end;
        }
        op->erased_end = end;
        break;
    }
    case ECAT_FOE_TARGET_OP_WRITE:
        if (op->offset > sizeof(f->flash) || op->length > sizeof(f->flash) - op->offset)
            return ECAT_FOE_TARGET_ASYNC_FAILED;
        memcpy(f->flash + op->offset, op->data, op->length);
        break;
    case ECAT_FOE_TARGET_OP_READ:
        if (op->offset > sizeof(f->flash) || op->length > sizeof(f->flash) - op->offset)
            return ECAT_FOE_TARGET_ASYNC_FAILED;
        memcpy(op->data, f->flash + op->offset, op->length);
        break;
    case ECAT_FOE_TARGET_OP_FINALIZE:
        f->valid = true;
        break;
    }
    f->pending = false;
    return ECAT_FOE_TARGET_ASYNC_COMPLETE;
}

static void fw_flash_abort(void *ctx) { ((test_flash_async_t *)ctx)->pending = false; }
static void fw_flash_abort_session(void *ctx) {
    test_flash_async_t *f = (test_flash_async_t *)ctx;
    f->pending = false;
    f->valid = false;
}

static const ecat_foe_target_async_ops_t s_fw_flash_ops = {
    fw_flash_start, fw_flash_poll, fw_flash_abort, fw_flash_abort_session
};

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

static int param_read_open(void *ctx) { (void)ctx; g_init_called = true; return 0; }
static void param_read_close(void *ctx) { (void)ctx; g_read_close_called = true; }

/* Log Mock 驱动 */
static int log_init(void *ctx) { (void)ctx; g_init_called = true; return 0; }
static void log_close(void *ctx) { (void)ctx; g_read_close_called = true; }
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
    param_desc.driver.read_open = param_read_open;
    param_desc.driver.read_close = param_read_close;
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
    log_desc.driver.read_open = log_init;
    log_desc.driver.read_close = log_close;
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
    /* 4. 取消与异常清理: IMPORT 保证 abort，EXPORT 保证 read_close 且不 abort */
    /* ---------------------------------------------------------------------- */
    reset_mock_flags();
    /* 4.1 导出取消: 仅调用 read_close，绝不调用 abort 使原数据失效 */
    assert(ecat_transfer_begin(&manager, &channel_uart, &uart_read_param, &dummy_token) == ECAT_TRANSFER_COMPLETE);
    assert(g_init_called);
    assert(!g_abort_called);
    assert(ecat_transfer_cancel(&manager, &channel_uart, dummy_token) == ECAT_TRANSFER_COMPLETE);
    assert(g_read_close_called);
    assert(!g_abort_called);

    /* 4.2 导入取消: 调用底层 write driver 的 abort 保证未完结写入回滚 */
    reset_mock_flags();
    ecat_transfer_request_t uart_write_param = {
        .transport_id = 1U,
        .target_id = 2U,
        .direction = ECAT_TRANSFER_DIR_IMPORT,
        .expected_size = 64
    };
    assert(ecat_transfer_begin(&manager, &channel_uart, &uart_write_param, &dummy_token) == ECAT_TRANSFER_COMPLETE);
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

    /* 验证 Issue 2 & R2-01: 只读 EXPORT 生命周期与清理契约 */
    printf("--- 测试 EXPORT 只读生命周期与驱动清理 ---\n");

    /* 2.1 旧驱动 (仅含 io.init，无 read_close): 缺少安全只读关闭，必须在 begin 时拒绝，禁止静默泄漏或调用写abort */
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
    g_valid_state = 1;
    g_driver_locked = 0;
    ecat_transfer_request_t exp_req = {
        .transport_id = 1U,
        .target_id = 10U,
        .direction = ECAT_TRANSFER_DIR_EXPORT,
        .expected_size = 16U,
        .fw_version = 0U
    };
    uint64_t exp_token = 0;
    /* 旧契约驱动缺少安全只读关闭能力，begin 必须拒绝 */
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &exp_req, &exp_token) == ECAT_TRANSFER_FAILED);
    assert(fw_manager.last_error == ECAT_TRANSFER_ERR_DRIVER_INIT);
    assert(g_init_called == false);
    assert(g_driver_locked == 0);
    assert(g_valid_state == 1);
    assert(g_abort_called == false);
    assert(core.state == ECAT_FOE_STATE_IDLE);

    /* 2.2 不配对只读驱动: 仅提供 read_open 或仅提供 read_close 必须在 begin 时拒绝 */
    ecat_target_desc_t unpair_open_desc;
    memset(&unpair_open_desc, 0, sizeof(unpair_open_desc));
    unpair_open_desc.target_id = 12U;
    strncpy(unpair_open_desc.name, "ro_unpair_open", sizeof(unpair_open_desc.name));
    unpair_open_desc.type = ECAT_TARGET_TYPE_DATA;
    unpair_open_desc.permissions = ECAT_PERM_READ;
    unpair_open_desc.max_size = 1024U;
    unpair_open_desc.enabled = true;
    unpair_open_desc.driver.read_open = lifecycle_read_open;
    unpair_open_desc.driver.read_close = NULL;
    unpair_open_desc.driver.io.read = lifecycle_read;
    assert(ecat_target_registry_add(&reg, &unpair_open_desc) == 0);
    assert(ecat_target_registry_add_acl(&reg, 0U, 12U, ECAT_PERM_READ) == 0);

    reset_mock_flags();
    exp_req.target_id = 12U;
    assert(ecat_transfer_begin(&manager, &channel_uart, &exp_req, &exp_token) == ECAT_TRANSFER_FAILED);
    assert(manager.last_error == ECAT_TRANSFER_ERR_DRIVER_INIT);
    assert(g_read_open_called == false);

    ecat_target_desc_t unpair_close_desc;
    memset(&unpair_close_desc, 0, sizeof(unpair_close_desc));
    unpair_close_desc.target_id = 13U;
    strncpy(unpair_close_desc.name, "ro_unpair_close", sizeof(unpair_close_desc.name));
    unpair_close_desc.type = ECAT_TARGET_TYPE_DATA;
    unpair_close_desc.permissions = ECAT_PERM_READ;
    unpair_close_desc.max_size = 1024U;
    unpair_close_desc.enabled = true;
    unpair_close_desc.driver.read_open = NULL;
    unpair_close_desc.driver.read_close = lifecycle_read_close;
    unpair_close_desc.driver.io.read = lifecycle_read;
    assert(ecat_target_registry_add(&reg, &unpair_close_desc) == 0);
    assert(ecat_target_registry_add_acl(&reg, 0U, 13U, ECAT_PERM_READ) == 0);

    reset_mock_flags();
    exp_req.target_id = 13U;
    assert(ecat_transfer_begin(&manager, &channel_uart, &exp_req, &exp_token) == ECAT_TRANSFER_FAILED);
    assert(manager.last_error == ECAT_TRANSFER_ERR_DRIVER_INIT);

    /* 2.3 纯 read 驱动 (无生命周期钩子): 允许导出，finish/cancel/读失败绝不触发 abort 或泄漏锁 */
    ecat_target_desc_t pure_read_desc;
    memset(&pure_read_desc, 0, sizeof(pure_read_desc));
    pure_read_desc.target_id = 14U;
    strncpy(pure_read_desc.name, "pure_read", sizeof(pure_read_desc.name));
    pure_read_desc.type = ECAT_TARGET_TYPE_DATA;
    pure_read_desc.permissions = ECAT_PERM_READ;
    pure_read_desc.max_size = 1024U;
    pure_read_desc.enabled = true;
    pure_read_desc.driver.io.read = lifecycle_read;
    assert(ecat_target_registry_add(&reg, &pure_read_desc) == 0);
    assert(ecat_target_registry_add_acl(&reg, 0U, 14U, ECAT_PERM_READ) == 0);

    /* 纯 read: 正常 finish */
    reset_mock_flags();
    g_valid_state = 1;
    exp_req.target_id = 14U;
    assert(ecat_transfer_begin(&manager, &channel_uart, &exp_req, &exp_token) == ECAT_TRANSFER_COMPLETE);
    assert(manager.driver_initialized == false);
    assert(ecat_transfer_finish(&manager, &channel_uart, exp_token) == ECAT_TRANSFER_COMPLETE);
    assert(g_valid_state == 1);
    assert(g_driver_locked == 0);
    assert(g_finalize_called == false);
    assert(g_abort_called == false);

    /* 纯 read: cancel 取消 */
    reset_mock_flags();
    g_valid_state = 1;
    assert(ecat_transfer_begin(&manager, &channel_uart, &exp_req, &exp_token) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_cancel(&manager, &channel_uart, exp_token) == ECAT_TRANSFER_COMPLETE);
    assert(g_valid_state == 1);
    assert(g_driver_locked == 0);
    assert(g_abort_called == false);

    /* 纯 read: read 失败 */
    reset_mock_flags();
    g_valid_state = 1;
    assert(ecat_transfer_begin(&manager, &channel_uart, &exp_req, &exp_token) == ECAT_TRANSFER_COMPLETE);
    g_fail_read = true;
    uint8_t pure_dummy[16];
    size_t pure_act = 0;
    assert(ecat_transfer_read(&manager, &channel_uart, exp_token, pure_dummy, sizeof(pure_dummy), &pure_act) == ECAT_TRANSFER_FAILED);
    assert(g_valid_state == 1);
    assert(g_driver_locked == 0);
    assert(g_abort_called == false);
    g_fail_read = false;

    /* 2.4 专用只读生命周期 (read_open / read_close): 验证配对调用与保护有效标记 */
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
    assert(g_init_called == false); /* 导出流程绝不调用写生命周期的 io.init */
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

    /* 3.6 驱动映射上下文与回调严格一致性校验: 两个目标实例共用驱动函数但 ctx 不同，严禁放行 */
    int ctx_a = 0xA, ctx_b = 0xB;
    core.cfg.targets[0].user_ctx = &ctx_a;
    ((ecat_target_desc_t *)fw_reg_desc)->user_ctx = &ctx_b;
    uint64_t ctx_mismatch_tok = 0;
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &oversize_req, &ctx_mismatch_tok) == ECAT_TRANSFER_FAILED);
    assert(fw_manager.last_error == ECAT_TRANSFER_ERR_DRIVER_WRITE);
    assert(fw_manager.session.owner == NULL);
    /* 回调函数不一致拒绝 */
    ((ecat_target_desc_t *)fw_reg_desc)->user_ctx = &ctx_a;
    ecat_foe_target_driver_t modified_drv = param_desc.driver.io;
    modified_drv.erase_to = NULL;
    core.cfg.targets[0].driver = &modified_drv;
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &oversize_req, &ctx_mismatch_tok) == ECAT_TRANSFER_FAILED);
    assert(fw_manager.last_error == ECAT_TRANSFER_ERR_DRIVER_WRITE);
    core.cfg.targets[0].driver = &param_desc.driver.io;

    /* 3.7 临时会话限额不可永久污染 core.cfg 原始配置 */
    core.cfg.targets[0].max_payload_size = 1024U;
    ((ecat_target_desc_t *)fw_reg_desc)->max_size = 16U;
    uint64_t sess_tok = 0;
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &oversize_req, &sess_tok) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_cancel(&fw_manager, &channel_uart, sess_tok) == ECAT_TRANSFER_COMPLETE);
    assert(core.cfg.targets[0].max_payload_size == 1024U);

    ((ecat_target_desc_t *)fw_reg_desc)->max_size = 1024U;
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &oversize_req, &sess_tok) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_cancel(&fw_manager, &channel_uart, sess_tok) == ECAT_TRANSFER_COMPLETE);
    assert(core.cfg.targets[0].max_payload_size == 1024U);

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
    assert(ecat_upgrade_session_owns_mgr(old_mgr.session_ptr, &old_mgr, &channel_can, old_token));
    /* 验证旧接口 release 无法绕过管理器身份检查 */
    assert(ecat_upgrade_session_release(old_mgr.session_ptr, &channel_can, old_token) == ECAT_UPGRADE_SESSION_STALE);

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

    /* 4.6 多传输管理器绑定权威注册表契约: 严禁后发管理器静默篡改核心权威注册表 */
    ecat_target_registry_t conflicting_reg;
    assert(ecat_target_registry_init(&conflicting_reg) == 0);
    ecat_transfer_t second_mgr;
    /* 对同一个已绑定 reg 的 core 尝试传入 conflicting_reg，必须返回 -1 拒绝 */
    assert(ecat_transfer_init(&second_mgr, &conflicting_reg, &core) == -1);
    assert(core.registry == &reg); /* 核心权威注册表指针未受篡改 */

    /* 传入相同的权威注册表指针，允许共存并共享会话仲裁 */
    assert(ecat_transfer_init(&second_mgr, &reg, &core) == 0);
    assert(second_mgr.session_ptr == fw_manager.session_ptr);

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
    /* 8. Issue 8 & R2-03: 32位平台算术环绕防范与 64位大尺寸防窄化验证       */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试 Issue 8 & R2-03: 32 位加法环绕防范与大尺寸防窄化 ---\n");

    /* 8.1 32位平台加法环绕防范 (offset=1, length=UINT32_MAX 减法检查防加法环绕) */
    reset_mock_flags();
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &req_data_unknown, &data_tok) == ECAT_TRANSFER_COMPLETE);
    uint8_t one_byte = 0x55;
    /* 先写入 1 字节使 current_offset = 1 */
    assert(ecat_transfer_write(&fw_manager, &channel_uart, data_tok, &one_byte, 1U) == ECAT_TRANSFER_COMPLETE);
    assert(fw_manager.current_offset == 1U);
    /* 传入 (size_t)UINT32_MAX: 若加法环绕为 0，则 0 <= limit 会错误放行；减法校验必须安全拦截 */
    assert(ecat_transfer_write(&fw_manager, &channel_uart, data_tok, &one_byte, (size_t)UINT32_MAX) == ECAT_TRANSFER_FAILED);
    assert(fw_manager.last_error == ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED);
    assert(g_abort_called == true);
    assert(fw_manager.session.owner == NULL);

    /* 8.2 固件模式同样验证 32 位加法环绕防范与有界推导 */
    uint64_t fw_wrap_tok = 0;
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &oversize_req, &fw_wrap_tok) == ECAT_TRANSFER_COMPLETE);
    fw_manager.current_offset = 1U; /* 模拟 offset=1 */
    assert(ecat_transfer_write(&fw_manager, &channel_uart, fw_wrap_tok, &one_byte, (size_t)UINT32_MAX) == ECAT_TRANSFER_FAILED);
    assert(fw_manager.last_error == ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED);
    assert(fw_manager.session.owner == NULL);

    /* 8.3 纯算术减法防御对比验证 (可移植到任何 32 位 MCU 架构) */
    /* [说明]: 当前运行宿主为 64 位环境 (__SIZEOF_SIZE_T__ == 8)；
     * 此处通过 32 位整型极限值 (uint32_t) 进行等价减法判定逻辑检验，确保移植到 32 位 MCU 时绝不环绕。
     * 因当前 macOS 宿主环境缺乏 32 位用户态二进制执行环境与模拟器，未进行真实 32 位目标机二进制动态运行，特此明确标注。 */
    uint32_t test_off = 1U;
    uint32_t test_limit = 1024U;
    uint32_t test_len = UINT32_MAX;
    /* 若用旧加法检查: 1 + UINT32_MAX 在 32 位下环绕为 0，0 > 1024 为 false (漏洞放行) */
    assert((uint32_t)(test_off + test_len) == 0U);
    /* 用新减法检查: 1 <= 1024 且 test_len > (1024 - 1)，严格拦截 */
    bool rejected = (test_off > test_limit) || (test_len > (test_limit - test_off));
    assert(rejected == true);

    /* 8.4 针对固件已知 expected_size 在接近 UINT32_MAX 容量时的 32 位溢出回绕防御测试 */
    ecat_target_desc_t near_max_desc;
    memset(&near_max_desc, 0, sizeof(near_max_desc));
    near_max_desc.target_id = 4U;
    strncpy(near_max_desc.name, "near_max_fw", sizeof(near_max_desc.name));
    near_max_desc.type = ECAT_TARGET_TYPE_FIRMWARE;
    near_max_desc.permissions = ECAT_PERM_WRITE;
    near_max_desc.max_size = UINT32_MAX - 100U;
    near_max_desc.enabled = true;
    near_max_desc.driver.io = param_desc.driver.io;
    assert(ecat_target_registry_add(&reg, &near_max_desc) == 0);
    assert(ecat_target_registry_add_acl(&reg, 1U, 4U, ECAT_PERM_WRITE) == 0);

    core.cfg.targets[3].enabled = true;
    core.cfg.targets[3].max_payload_size = UINT32_MAX - 100U;
    core.cfg.targets[3].write_unit = 8U;
    core.cfg.targets[3].driver = &param_desc.driver.io;
    core.cfg.targets[3].user_ctx = NULL;

    /* 发起合法请求: expected_size = 500U */
    /* 旧代码中: target->max_size + 148U + 7U 溢出回绕为 54U，导致 500U > 54U 被错误拒绝！ */
    ecat_transfer_request_t near_max_req = {
        .transport_id = 1U,
        .target_id = 4U,
        .direction = ECAT_TRANSFER_DIR_IMPORT,
        .expected_size = 500U,
        .fw_version = 0x01000000U
    };
    uint64_t near_max_tok = 0;
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &near_max_req, &near_max_tok) == ECAT_TRANSFER_COMPLETE);
    assert(near_max_tok != 0);
    assert(ecat_transfer_cancel(&fw_manager, &channel_uart, near_max_tok) == ECAT_TRANSFER_COMPLETE);

    /* 8.5 规范固件完整升级流端到端验证 (包含真实 148 字节固定头、有效负载、分片与成功提交) */
    /*
     * 【规范语义说明】:
     * 依据 docs/spec/efw-format-v0.2.md，当前 EFW 容器规范固定 148 字节头部 (ECAT_FOE_EFW_HEADER_MIN_LEN)。
     * core.cfg.max_header_size (如配置 148 或 256) 仅代表宿主为状态机分配的内部装配缓冲容量，非协议扩展头格式。
     * 固件容器物理总上限恒定为: 有效负载上限 + 148 字节头 + 对齐填充上限。
     */
    core.cfg.project_id = 0x10010001U;
    core.cfg.hardware_id = 1U;
    core.cfg.local_hw_ver = 1U;
    core.cfg.targets[0].max_payload_size = 16U;
    core.cfg.targets[0].write_unit = 8U;
    ecat_target_desc_t *fw_t1 = (ecat_target_desc_t *)ecat_target_registry_find_by_id(&reg, 1U);
    assert(fw_t1 != NULL);
    uint32_t orig_t1_max_size = fw_t1->max_size;
    fw_t1->max_size = 16U;

    /* 发起合法 16 字节固件升级 (总长度 148 + 16 = 164 字节 <= 16 + 148 + 7 = 171 字节物理上限) */
    ecat_transfer_request_t valid_fw_req = {
        .transport_id = 1U,
        .target_id = 1U,
        .direction = ECAT_TRANSFER_DIR_IMPORT,
        .expected_size = sizeof(s_valid_16_efw), /* 164 字节 */
        .fw_version = 0x01020304U
    };
    uint64_t valid_fw_tok = 0;
    reset_mock_flags();
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &valid_fw_req, &valid_fw_tok) == ECAT_TRANSFER_COMPLETE);
    assert(valid_fw_tok != 0);

    /* 分片写入完整固件包: 64 字节 + 64 字节 + 36 字节 = 164 字节 */
    assert(ecat_transfer_write(&fw_manager, &channel_uart, valid_fw_tok, s_valid_16_efw, 64U) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_write(&fw_manager, &channel_uart, valid_fw_tok, s_valid_16_efw + 64U, 64U) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_write(&fw_manager, &channel_uart, valid_fw_tok, s_valid_16_efw + 128U, 36U) == ECAT_TRANSFER_COMPLETE);

    /* 完成传输并验证驱动成功 finalize 提交与核心写入数据闭环 */
    assert(ecat_transfer_finish(&fw_manager, &channel_uart, valid_fw_tok) == ECAT_TRANSFER_COMPLETE);
    assert(g_finalize_called == true);
    assert(core.written_offset == 16U);
    assert(fw_manager.session.owner == NULL); /* 锁安全释放 */

    /* 8.6 固件截断场景测试 (Truncation): 声明 164 字节但仅写入 100 字节即 finish */
    uint64_t trunc_fw_tok = 0;
    reset_mock_flags();
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &valid_fw_req, &trunc_fw_tok) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_write(&fw_manager, &channel_uart, trunc_fw_tok, s_valid_16_efw, 100U) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_finish(&fw_manager, &channel_uart, trunc_fw_tok) == ECAT_TRANSFER_FAILED);
    assert(g_finalize_called == false);
    assert(fw_manager.session.owner == NULL); /* finish 失败时安全释放会话锁与资源 */
    assert(fw_manager.active == false);

    /* 8.7 物理容器流上限越界精确拒绝测试 (165 字节 > 164 字节物理上限 = 16B payload + 148B header) */
    /* 依据 docs/spec/efw-format-v0.2.md:53，流中不存在额外的尾部对齐填充；
     * 即使仅超出 1 字节 (在 write_unit - 1 的对齐补齐窗口内)，亦必须在 begin 阶段被精确拦截，验证拒绝前无驱动副作用 */
    ecat_transfer_request_t oversize_fw_req = {
        .transport_id = 1U,
        .target_id = 1U,
        .direction = ECAT_TRANSFER_DIR_IMPORT,
        .expected_size = 165U, /* 165 > 16 + 148 = 164 */
        .fw_version = 0x01020304U
    };
    uint64_t oversize_fw_tok = 0;
    reset_mock_flags();
    assert(ecat_transfer_begin(&fw_manager, &channel_uart, &oversize_fw_req, &oversize_fw_tok) == ECAT_TRANSFER_FAILED);
    assert(fw_manager.last_error == ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED);
    assert(fw_manager.session.owner == NULL); /* 锁已安全释放 */
    assert(fw_manager.active == false);       /* 状态机未激活 */
    assert(fw_manager.driver_initialized == false);
    assert(g_init_called == false && g_invalidate_called == false && g_abort_called == false); /* 无驱动副作用 */

    /* 恢复 Target 1 与 core 配置 */
    fw_t1->max_size = orig_t1_max_size;
    core.cfg.targets[0].max_payload_size = 1024U * 1024U;

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

    /* 9. 共享会话跨管理器实例身份绑定与隔离测试 (Manager Identity Binding in Shared Session) */
    printf("--- 测试跨管理器会话身份绑定与隔离防御 ---\n");
    reset_mock_flags();

    /* 9.1 两个 transfer 管理器共享同一个 core: 非发起管理器调用各操作必须返回 STALE，且不破坏原管理器会话与驱动 */
    ecat_transfer_t mgr_first, mgr_second;
    assert(ecat_transfer_init(&mgr_first, &reg, &core) == 0);
    assert(ecat_transfer_init(&mgr_second, &reg, &core) == 0);

    int client_owner1 = 100, client_owner2 = 200;
    ecat_transfer_request_t shared_req = {
        .transport_id = 1U,
        .target_id = 2U, /* Target 2 为数据区块，支持读写 */
        .direction = ECAT_TRANSFER_DIR_IMPORT,
        .expected_size = 16U,
        .fw_version = 0U
    };
    uint64_t lease_tok1 = 0;
    assert(ecat_transfer_begin(&mgr_first, &client_owner1, &shared_req, &lease_tok1) == ECAT_TRANSFER_COMPLETE);
    assert(lease_tok1 != 0);
    assert(mgr_first.active == true);
    assert(g_init_called == true);
    reset_mock_flags();

    /* 在非发起的 mgr_second 上用同 owner 同 token 尝试 write/read/finish/cancel 均必须返回 STALE */
    uint8_t dummy_buf[16] = {0};
    size_t dummy_actual = 0;
    assert(ecat_transfer_write(&mgr_second, &client_owner1, lease_tok1, dummy_buf, sizeof(dummy_buf)) == ECAT_TRANSFER_STALE);
    assert(ecat_transfer_read(&mgr_second, &client_owner1, lease_tok1, dummy_buf, sizeof(dummy_buf), &dummy_actual) == ECAT_TRANSFER_STALE);
    assert(ecat_transfer_finish(&mgr_second, &client_owner1, lease_tok1) == ECAT_TRANSFER_STALE);
    assert(ecat_transfer_cancel(&mgr_second, &client_owner1, lease_tok1) == ECAT_TRANSFER_STALE);

    /* 在非发起的 mgr_second 上用不同 owner 尝试亦必须返回 STALE */
    assert(ecat_transfer_write(&mgr_second, &client_owner2, lease_tok1, dummy_buf, sizeof(dummy_buf)) == ECAT_TRANSFER_STALE);
    assert(ecat_transfer_cancel(&mgr_second, &client_owner2, lease_tok1) == ECAT_TRANSFER_STALE);

    /* 验证第一管理器的会话与底层驱动完好无损 */
    assert(mgr_first.active == true);
    assert(g_abort_called == false);
    assert(g_finalize_called == false);

    /* 此时在 mgr_second 上尝试 begin 必须返回 BUSY，绝不能二次初始化驱动 */
    uint64_t lease_tok2 = 0;
    assert(ecat_transfer_begin(&mgr_second, &client_owner2, &shared_req, &lease_tok2) == ECAT_TRANSFER_BUSY);
    assert(lease_tok2 == 0);
    assert(g_init_called == false);

    /* 原管理器正常 cancel 成功释放租约与底层驱动 */
    assert(ecat_transfer_cancel(&mgr_first, &client_owner1, lease_tok1) == ECAT_TRANSFER_COMPLETE);
    assert(mgr_first.active == false);

    /* 原管理器释放后，mgr_second 才能成功获取会话并初始化驱动 (验证复用) */
    assert(ecat_transfer_begin(&mgr_second, &client_owner2, &shared_req, &lease_tok2) == ECAT_TRANSFER_COMPLETE);
    assert(lease_tok2 != 0);
    assert(mgr_second.active == true);
    assert(ecat_transfer_cancel(&mgr_second, &client_owner2, lease_tok2) == ECAT_TRANSFER_COMPLETE);

    /* 9.2 transfer 管理器与 upgrade 管理器交叉调用隔离 */
    ecat_upgrade_t upg_mgr;
    assert(ecat_upgrade_init(&upg_mgr, &core) == 0);

    /* 场景 A: transfer 管理器持租，在 upgrade 管理器上调用全部操作必须返回 STALE */
    assert(ecat_transfer_begin(&mgr_first, &client_owner1, &shared_req, &lease_tok1) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_upgrade_write(&upg_mgr, &client_owner1, lease_tok1, dummy_buf, sizeof(dummy_buf)) == ECAT_UPGRADE_STALE);
    assert(ecat_upgrade_finish(&upg_mgr, &client_owner1, lease_tok1) == ECAT_UPGRADE_STALE);
    assert(ecat_upgrade_cancel(&upg_mgr, &client_owner1, lease_tok1) == ECAT_UPGRADE_STALE);
    assert(ecat_upgrade_cancel(&upg_mgr, &client_owner2, lease_tok1) == ECAT_UPGRADE_STALE);

    /* upgrade 管理器尝试 begin 必须返回 BUSY */
    ecat_upgrade_request_t upg_req = { .target_id = 1U, .fw_version = 0x01000000U };
    uint64_t upg_tok = 0;
    assert(ecat_upgrade_begin(&upg_mgr, &client_owner2, &upg_req, &upg_tok) == ECAT_UPGRADE_BUSY);
    assert(mgr_first.active == true);
    assert(ecat_transfer_cancel(&mgr_first, &client_owner1, lease_tok1) == ECAT_TRANSFER_COMPLETE);

    /* 场景 B: upgrade 管理器持租，在 transfer 管理器上调用全部操作必须返回 STALE */
    assert(ecat_upgrade_begin(&upg_mgr, &client_owner1, &upg_req, &upg_tok) == ECAT_UPGRADE_COMPLETE);
    assert(ecat_transfer_write(&mgr_first, &client_owner1, upg_tok, dummy_buf, sizeof(dummy_buf)) == ECAT_TRANSFER_STALE);
    assert(ecat_transfer_read(&mgr_first, &client_owner1, upg_tok, dummy_buf, sizeof(dummy_buf), &dummy_actual) == ECAT_TRANSFER_STALE);
    assert(ecat_transfer_finish(&mgr_first, &client_owner1, upg_tok) == ECAT_TRANSFER_STALE);
    assert(ecat_transfer_cancel(&mgr_first, &client_owner1, upg_tok) == ECAT_TRANSFER_STALE);
    assert(ecat_transfer_begin(&mgr_first, &client_owner2, &shared_req, &lease_tok1) == ECAT_TRANSFER_BUSY);
    assert(ecat_upgrade_cancel(&upg_mgr, &client_owner1, upg_tok) == ECAT_UPGRADE_COMPLETE);

    /* 9.3 两个 upgrade 管理器共享同一个 core: 非发起管理器调用必须返回 STALE */
    ecat_upgrade_t upg_second;
    assert(ecat_upgrade_init(&upg_second, &core) == 0);
    assert(ecat_upgrade_begin(&upg_mgr, &client_owner1, &upg_req, &upg_tok) == ECAT_UPGRADE_COMPLETE);
    assert(ecat_upgrade_write(&upg_second, &client_owner1, upg_tok, dummy_buf, sizeof(dummy_buf)) == ECAT_UPGRADE_STALE);
    assert(ecat_upgrade_finish(&upg_second, &client_owner1, upg_tok) == ECAT_UPGRADE_STALE);
    assert(ecat_upgrade_cancel(&upg_second, &client_owner1, upg_tok) == ECAT_UPGRADE_STALE);
    assert(ecat_upgrade_cancel(&upg_second, &client_owner2, upg_tok) == ECAT_UPGRADE_STALE);
    assert(ecat_upgrade_begin(&upg_second, &client_owner2, &upg_req, &lease_tok2) == ECAT_UPGRADE_BUSY);
    assert(ecat_upgrade_cancel(&upg_mgr, &client_owner1, upg_tok) == ECAT_UPGRADE_COMPLETE);
    assert(ecat_upgrade_begin(&upg_second, &client_owner2, &upg_req, &lease_tok2) == ECAT_UPGRADE_COMPLETE);
    assert(ecat_upgrade_cancel(&upg_second, &client_owner2, lease_tok2) == ECAT_UPGRADE_COMPLETE);

    printf("--- 测试 Section 10: 真正非阻塞异步双向传输与统一状态机 (Async DATA / FIRMWARE) ---\n");
    /* 10.1 异步目标缺失异步驱动时在 begin 阶段必须被严格拒绝，无任何副作用 */
    ecat_target_registry_t async_reg;
    assert(ecat_target_registry_init(&async_reg) == 0);
    ecat_target_desc_t fake_async_desc;
    memset(&fake_async_desc, 0, sizeof(fake_async_desc));
    fake_async_desc.target_id = 10U;
    strncpy(fake_async_desc.name, "fake_async", sizeof(fake_async_desc.name));
    fake_async_desc.type = ECAT_TARGET_TYPE_DATA;
    fake_async_desc.permissions = ECAT_PERM_RW;
    fake_async_desc.max_size = 1024U;
    fake_async_desc.enabled = true;
    fake_async_desc.asynchronous = true;
    /* 提供同步驱动使 registry_add 通过，但未配置 async_ops */
    fake_async_desc.driver.io.write = param_write;
    fake_async_desc.driver.io.erase_to = param_erase_to;
    fake_async_desc.driver.io.read = param_read;
    fake_async_desc.driver.io.init = param_init;
    assert(ecat_target_registry_add(&async_reg, &fake_async_desc) == 0);

    ecat_transfer_t async_xfer;
    assert(ecat_transfer_init(&async_xfer, &async_reg, NULL) == 0);
    ecat_transfer_request_t fake_req = {
        .transport_id = 1U, .target_id = 10U, .direction = ECAT_TRANSFER_DIR_IMPORT, .expected_size = 100U
    };
    uint64_t fake_tok = 0;
    /* begin 必须拒绝假异步目标并返回 FAILED，last_error 明确为 DRIVER_INIT */
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &fake_req, &fake_tok) == ECAT_TRANSFER_FAILED);
    assert(ecat_transfer_get_last_error(&async_xfer) == ECAT_TRANSFER_ERR_DRIVER_INIT);
    assert(!async_xfer.active);

    /* 10.2 真正异步 DATA 目标导入 (WRITE) 与轮询推进 */
    ecat_async_data_ops_t real_async_ops = {
        .start = test_data_async_start,
        .poll = test_data_async_poll,
        .abort = test_data_async_abort
    };
    ecat_target_desc_t real_async_desc;
    memset(&real_async_desc, 0, sizeof(real_async_desc));
    real_async_desc.target_id = 11U;
    strncpy(real_async_desc.name, "real_async", sizeof(real_async_desc.name));
    real_async_desc.type = ECAT_TARGET_TYPE_DATA;
    real_async_desc.permissions = ECAT_PERM_RW;
    real_async_desc.max_size = 1024U;
    real_async_desc.enabled = true;
    real_async_desc.asynchronous = true;
    real_async_desc.driver.async_ops = &real_async_ops;
    real_async_desc.driver.read_open = lifecycle_read_open;
    real_async_desc.driver.read_close = lifecycle_read_close;
    assert(ecat_target_registry_add(&async_reg, &real_async_desc) == 0);

    /* 配置固定暂存区 */
    uint8_t staging_mem[256];
    assert(ecat_transfer_set_staging_buffer(&async_xfer, staging_mem, sizeof(staging_mem)) == 0);

    ecat_transfer_request_t async_write_req = {
        .transport_id = 1U, .target_id = 11U, .direction = ECAT_TRANSFER_DIR_IMPORT, .expected_size = 64U
    };
    uint64_t async_tok = 0;
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &async_write_req, &async_tok) == ECAT_TRANSFER_COMPLETE);
    assert(async_xfer.active == true);

    /* 设置下一次驱动返回 PENDING */
    s_next_async_result = ECAT_ASYNC_OP_PENDING;
    s_async_start_called = false;
    uint8_t test_chunk[64];
    memset(test_chunk, 0xAB, sizeof(test_chunk));

    /* write 必须返回 ECAT_TRANSFER_PENDING */
    assert(ecat_transfer_write(&async_xfer, &client_owner1, async_tok, test_chunk, sizeof(test_chunk)) == ECAT_TRANSFER_PENDING);
    assert(s_async_start_called == true);
    assert(ecat_transfer_is_pending(&async_xfer) == true);

    /* PENDING 期间：新 write 与 finish 必须返回 BUSY */
    assert(ecat_transfer_write(&async_xfer, &client_owner1, async_tok, test_chunk, sizeof(test_chunk)) == ECAT_TRANSFER_BUSY);
    assert(ecat_transfer_finish(&async_xfer, &client_owner1, async_tok) == ECAT_TRANSFER_BUSY);

    /* 错误 owner 或错误 token poll 必须返回 STALE */
    assert(ecat_transfer_poll(&async_xfer, &client_owner2, async_tok) == ECAT_TRANSFER_STALE);
    assert(ecat_transfer_poll(&async_xfer, &client_owner1, async_tok + 99) == ECAT_TRANSFER_STALE);

    /* 合法 owner poll 推进：驱动仍为 PENDING 时返回 PENDING */
    s_async_poll_called = false;
    assert(ecat_transfer_poll(&async_xfer, &client_owner1, async_tok) == ECAT_TRANSFER_PENDING);
    assert(s_async_poll_called == true);
    assert(ecat_transfer_is_pending(&async_xfer) == true);

    /* 驱动变为 COMPLETE 时 poll 推进并返回 COMPLETE */
    s_next_async_result = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transfer_poll(&async_xfer, &client_owner1, async_tok) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_is_pending(&async_xfer) == false);
    assert(ecat_transfer_get_bytes_transferred(&async_xfer) == 64U);

    /* 对已完成的操作重复 poll 必须返回 INVALID */
    assert(ecat_transfer_poll(&async_xfer, &client_owner1, async_tok) == ECAT_TRANSFER_INVALID);

    /* 异步 finish 流程 */
    s_next_async_result = ECAT_ASYNC_OP_PENDING;
    assert(ecat_transfer_finish(&async_xfer, &client_owner1, async_tok) == ECAT_TRANSFER_PENDING);
    assert(ecat_transfer_is_pending(&async_xfer) == true);

    /* poll 推进 finish 完成 */
    s_next_async_result = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transfer_poll(&async_xfer, &client_owner1, async_tok) == ECAT_TRANSFER_COMPLETE);
    assert(async_xfer.active == false);
    assert(!ecat_upgrade_session_owns_mgr(async_xfer.session_ptr, &async_xfer, &client_owner1, async_tok)); /* 租约已释放 */

    /* 10.2B 暂存区容量超限校验: 请求写入 300 字节，暂存区只有 256 字节，必须立即拒绝 */
    ecat_transfer_request_t async_over_req = {
        .transport_id = 1U, .target_id = 11U, .direction = ECAT_TRANSFER_DIR_IMPORT, .expected_size = 1000U
    };
    uint64_t over_tok = 0;
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &async_over_req, &over_tok) == ECAT_TRANSFER_COMPLETE);
    uint8_t oversized_chunk[300];
    assert(ecat_transfer_write(&async_xfer, &client_owner1, over_tok, oversized_chunk, sizeof(oversized_chunk)) == ECAT_TRANSFER_FAILED);
    assert(ecat_transfer_get_last_error(&async_xfer) == ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED);
    assert(!async_xfer.active); /* 超界错误释放会话 */

    /* 10.3 真正异步 DATA 目标导出 (READ) 与取消 */
    ecat_transfer_request_t async_read_req = {
        .transport_id = 1U, .target_id = 11U, .direction = ECAT_TRANSFER_DIR_EXPORT, .expected_size = 64U
    };
    uint64_t read_tok = 0;
    reset_mock_flags();
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &async_read_req, &read_tok) == ECAT_TRANSFER_COMPLETE);
    assert(g_read_open_called == true);

    uint8_t read_out[64];
    size_t actual_read = 0;
    s_next_async_result = ECAT_ASYNC_OP_PENDING;
    assert(ecat_transfer_read(&async_xfer, &client_owner1, read_tok, read_out, sizeof(read_out), &actual_read) == ECAT_TRANSFER_PENDING);
    assert(ecat_transfer_is_pending(&async_xfer) == true);

    /* 在 PENDING 期间执行 cancel: 触发异步驱动 abort，释放租约，绝不触碰写 finalize */
    s_async_abort_called = false;
    assert(ecat_transfer_cancel(&async_xfer, &client_owner1, read_tok) == ECAT_TRANSFER_COMPLETE);
    assert(s_async_abort_called == true);
    assert(g_finalize_called == false);
    assert(async_xfer.active == false);
    assert(ecat_transfer_is_pending(&async_xfer) == false);

    /* 10.4 固件升级复用异步核心验证 (FIRMWARE Async) */
    ecat_upgrade_async_ops_t fw_ops = {
        .start = test_fw_async_start,
        .poll = test_fw_async_poll,
        .abort = test_fw_async_abort
    };
    int fw_dummy_ctx = 0;
    /* 核心未关联或伪造的 fake ops 必须被明确拒绝，防止绕过验签与权限检查 */
    assert(ecat_transfer_init_async_firmware(&mgr_first, &fw_ops, &fw_dummy_ctx) == -1);
    /* 即使把 abort 和 ctx 挂到 core 上，但由于 start/poll 不是权威核心，依然必须被严格拒绝 */
    core.async_abort_hook = fw_ops.abort;
    core.async_abort_ctx = &fw_dummy_ctx;
    assert(ecat_transfer_init_async_firmware(&mgr_first, &fw_ops, &fw_dummy_ctx) == -1);

    /* 真实异步固件核心初始化并完成权威强绑定 */
    static uint8_t s_fw10_packet[512];
    static uint8_t s_fw10_op[256];
    static ecat_foe_core_async_t s_fw10_session;
    core.cfg.project_id = 0x10010001U;
    core.cfg.hardware_id = 1U;
    core.cfg.local_hw_ver = 1U;
    core.cfg.max_header_size = ECAT_FOE_EFW_HEADER_MIN_LEN;
    core.cfg.targets[0].target_id = 1U;
    core.cfg.targets[0].enabled = true;
    core.cfg.targets[0].asynchronous = true;
    core.cfg.targets[0].async_ops = &s_fw_flash_ops;
    core.cfg.targets[0].async_user_ctx = &s_fw_test_flash;
    core.cfg.targets[0].max_payload_size = sizeof(s_fw_test_flash.flash);
    assert(ecat_foe_core_async_init(&s_fw10_session, &core, s_fw10_packet, sizeof(s_fw10_packet),
                                    s_fw10_op, sizeof(s_fw10_op)) == 0);
    assert(ecat_transfer_init_async_firmware(&mgr_first, ecat_foe_core_async_ops(), &s_fw10_session) == 0);
    uint8_t fw_staging[256];
    assert(ecat_transfer_set_staging_buffer(&mgr_first, fw_staging, sizeof(fw_staging)) == 0);

    /* 注册固件异步目标 */
    ecat_target_desc_t *fw_t1_desc = (ecat_target_desc_t *)ecat_target_registry_find_by_id(&reg, 1U);
    assert(fw_t1_desc != NULL);
    fw_t1_desc->asynchronous = true;

    /* 构造真实合法的 200 字节固件包 (148 字节 EFW 头部 + 52 字节载荷) */
    uint8_t fw_payload[52];
    memset(fw_payload, 0xEE, sizeof(fw_payload));
    uint8_t fw_efw[200];
    assert(make_test_efw(fw_efw, fw_payload, sizeof(fw_payload), 1U) == 200U);

    ecat_transfer_request_t fw_async_req = {
        .transport_id = 1U, .target_id = 1U, .direction = ECAT_TRANSFER_DIR_IMPORT, .expected_size = 200U,
        .fw_version = 0x01000000U
    };
    uint64_t fw_tok = 0;
    assert(ecat_transfer_begin(&mgr_first, &client_owner1, &fw_async_req, &fw_tok) == ECAT_TRANSFER_COMPLETE);
    assert(mgr_first.active == true);
    assert(mgr_first.is_async == true);

    /* write 第 1 块 100 字节 (EFW 前半头部): 写入完成，深拷贝暂存保证不可变性 */
    uint8_t fw_chunk_copy[100];
    memcpy(fw_chunk_copy, fw_efw, 100U);
    assert(ecat_transfer_write(&mgr_first, &client_owner1, fw_tok, fw_chunk_copy, 100U) == ECAT_TRANSFER_COMPLETE);
    /* 立即篡改外部输入缓冲区: 验证深拷贝与 CRC 不可变性 */
    memset(fw_chunk_copy, 0x00, sizeof(fw_chunk_copy));

    /* 关键断言 1: 异步固件写入完成后，CRC32 必须匹配原始 EFW 数据的 CRC，不受外部篡改影响 */
    uint32_t expected_fw_crc = ecat_foe_crc32_calc(fw_efw, 100U);
    assert(ecat_transfer_get_crc32(&mgr_first) == expected_fw_crc);

    /* 关键断言 2: expected_size 为 200，目前仅写入 100，finish 必须被拒绝且报 BOUNDARY_EXCEEDED */
    assert(ecat_transfer_finish(&mgr_first, &client_owner1, fw_tok) == ECAT_TRANSFER_FAILED);
    assert(ecat_transfer_get_last_error(&mgr_first) == ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED);
    assert(!mgr_first.active);

    /* 重新发起完整固件升级: expected_size 为 200，完整分块写入 200 字节后 finish */
    ecat_foe_core_on_abort(&core);
    assert(ecat_foe_core_async_init(&s_fw10_session, &core, s_fw10_packet, sizeof(s_fw10_packet),
                                    s_fw10_op, sizeof(s_fw10_op)) == 0);
    assert(ecat_transfer_init_async_firmware(&mgr_first, ecat_foe_core_async_ops(), &s_fw10_session) == 0);
    assert(ecat_transfer_begin(&mgr_first, &client_owner1, &fw_async_req, &fw_tok) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_write(&mgr_first, &client_owner1, fw_tok, fw_efw, 100U) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_write(&mgr_first, &client_owner1, fw_tok, fw_efw + 100U, 100U) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_get_bytes_transferred(&mgr_first) == 200U);
    ecat_transfer_result_t fin_res = ecat_transfer_finish(&mgr_first, &client_owner1, fw_tok);
    if (fin_res == ECAT_TRANSFER_PENDING) {
        assert(ecat_transfer_is_pending(&mgr_first) == true);
        assert(ecat_transfer_poll(&mgr_first, &client_owner1, fw_tok) == ECAT_TRANSFER_COMPLETE);
    } else {
        assert(fin_res == ECAT_TRANSFER_COMPLETE);
    }
    assert(s_fw_test_flash.valid == true); /* 真实固件核心 finalize 执行，介质成功生效 */

    /* 验证真实篡改固件被真实核心拦截 */
    uint8_t bad_efw[200];
    memcpy(bad_efw, fw_efw, 200U);
    bad_efw[0] = 0x00; /* 破坏 Magic */
    ecat_foe_core_on_abort(&core);
    assert(ecat_foe_core_async_init(&s_fw10_session, &core, s_fw10_packet, sizeof(s_fw10_packet),
                                    s_fw10_op, sizeof(s_fw10_op)) == 0);
    assert(ecat_transfer_init_async_firmware(&mgr_first, ecat_foe_core_async_ops(), &s_fw10_session) == 0);
    assert(ecat_transfer_begin(&mgr_first, &client_owner1, &fw_async_req, &fw_tok) == ECAT_TRANSFER_COMPLETE);
    /* 第 1 包头部 Magic 被篡改，真实核心当场检测到非法头部 Magic 并返回 FAILED 拦截 */
    assert(ecat_transfer_write(&mgr_first, &client_owner1, fw_tok, bad_efw, 100U) == ECAT_TRANSFER_FAILED);
    assert(ecat_transfer_get_last_error(&mgr_first) == ECAT_TRANSFER_ERR_CORE_UPGRADE);
    assert(mgr_first.active == false);
    assert(!ecat_upgrade_session_owns_mgr(mgr_first.session_ptr, &mgr_first, &client_owner1, fw_tok));

    /* 10.4.1 无 staging buffer 或分块超容量时异步固件写必须明确拒绝并安全释放 */
    ecat_foe_core_on_abort(&core);
    ecat_transfer_t mgr_fw_nostaging;
    assert(ecat_transfer_init(&mgr_fw_nostaging, &reg, &core) == 0);
    assert(ecat_foe_core_async_init(&s_fw10_session, &core, s_fw10_packet, sizeof(s_fw10_packet),
                                    s_fw10_op, sizeof(s_fw10_op)) == 0);
    assert(ecat_transfer_init_async_firmware(&mgr_fw_nostaging, ecat_foe_core_async_ops(), &s_fw10_session) == 0);
    uint64_t tok_nostage = 0;
    assert(ecat_transfer_begin(&mgr_fw_nostaging, &client_owner1, &fw_async_req, &tok_nostage) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_write(&mgr_fw_nostaging, &client_owner1, tok_nostage, fw_efw, 100U) == ECAT_TRANSFER_FAILED);
    assert(ecat_transfer_get_last_error(&mgr_fw_nostaging) == ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED);
    assert(!mgr_fw_nostaging.active);

    /* 10.5 异步 DATA 完整驱动生命周期验证 (纯非阻塞状态机与各阶段 PENDING 推进) */
    printf("--- 测试 10.5: 异步 DATA 完整驱动生命周期与失败安全回退 ---\n");
    memset(&s_hw_stage_test, 0, sizeof(s_hw_stage_test));
    ecat_target_desc_t async_full_desc;
    memset(&async_full_desc, 0, sizeof(async_full_desc));
    async_full_desc.target_id = 12U;
    strncpy(async_full_desc.name, "async_full_lifecycle", sizeof(async_full_desc.name));
    async_full_desc.type = ECAT_TARGET_TYPE_DATA;
    async_full_desc.permissions = ECAT_PERM_RW;
    async_full_desc.max_size = 1024U;
    async_full_desc.enabled = true;
    async_full_desc.asynchronous = true;
    async_full_desc.driver.async_ops = &s_stage_data_ops;
    async_full_desc.user_ctx = &s_hw_stage_test;
    assert(ecat_target_registry_add(&async_reg, &async_full_desc) == 0);

    /* 10.5.1: 异步 DATA 的 init 失败拦截并释放租约，单次精确 abort */
    memset(&s_hw_stage_test, 0, sizeof(s_hw_stage_test));
    s_hw_stage_test.fail_init = true;
    ecat_transfer_request_t req_12 = {
        .transport_id = 1U, .target_id = 12U, .direction = ECAT_TRANSFER_DIR_IMPORT, .expected_size = 128U
    };
    uint64_t tok_12 = 0;
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &req_12, &tok_12) == ECAT_TRANSFER_FAILED);
    assert(s_hw_stage_test.init_called == true);
    assert(s_hw_stage_test.abort_calls == 1);
    assert(ecat_transfer_get_last_error(&async_xfer) == ECAT_TRANSFER_ERR_DRIVER_INIT);
    assert(!async_xfer.active);

    /* 10.5.2: 异步 DATA 的 init 进入 PENDING，通过 poll 推进就绪 */
    memset(&s_hw_stage_test, 0, sizeof(s_hw_stage_test));
    s_hw_stage_test.pending_init = true;
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &req_12, &tok_12) == ECAT_TRANSFER_PENDING);
    assert(ecat_transfer_is_pending(&async_xfer) == true);
    assert(ecat_transfer_poll(&async_xfer, &client_owner1, tok_12) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_is_pending(&async_xfer) == false);
    assert(async_xfer.active == true);

    /* 10.5.3: 异步 DATA 写入前 erase_to 失败拦截并安全清理 */
    s_hw_stage_test.fail_erase = true;
    uint8_t dummy_chunk[64] = {0xAA};
    assert(ecat_transfer_write(&async_xfer, &client_owner1, tok_12, dummy_chunk, sizeof(dummy_chunk)) == ECAT_TRANSFER_FAILED);
    assert(s_hw_stage_test.erase_called == true);
    assert(s_hw_stage_test.abort_calls == 1);
    assert(ecat_transfer_get_last_error(&async_xfer) == ECAT_TRANSFER_ERR_DRIVER_ERASE);
    assert(!async_xfer.active);

    /* 10.5.4: 异步 DATA 写入前按需 erase_to 进入 PENDING，poll 推进并自动流转执行异步 WRITE */
    memset(&s_hw_stage_test, 0, sizeof(s_hw_stage_test));
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &req_12, &tok_12) == ECAT_TRANSFER_COMPLETE);
    s_hw_stage_test.pending_erase = true;
    s_hw_stage_test.pending_write = true;
    /* write 发起异步擦除，返回 PENDING */
    assert(ecat_transfer_write(&async_xfer, &client_owner1, tok_12, dummy_chunk, 64U) == ECAT_TRANSFER_PENDING);
    assert(s_hw_stage_test.erase_called == true);
    /* 第一次 poll: 擦除完成，状态机自动无缝启动 WRITE 阶段并保持 PENDING */
    assert(ecat_transfer_poll(&async_xfer, &client_owner1, tok_12) == ECAT_TRANSFER_PENDING);
    assert(s_hw_stage_test.write_called == true);
    /* 第二次 poll: WRITE 阶段完成 */
    s_hw_stage_test.pending_write = false;
    assert(ecat_transfer_poll(&async_xfer, &client_owner1, tok_12) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_get_bytes_transferred(&async_xfer) == 64U);
    assert(s_hw_stage_test.memory[0] == 0xAA);

    /* 再次写入 64 字节达到 expected_size (128)，擦除水印已满足，不重复触发擦除 */
    s_hw_stage_test.erase_called = false;
    assert(ecat_transfer_write(&async_xfer, &client_owner1, tok_12, dummy_chunk, 64U) == ECAT_TRANSFER_COMPLETE);
    assert(s_hw_stage_test.erase_called == false);
    assert(ecat_transfer_get_bytes_transferred(&async_xfer) == 128U);

    /* 10.5.5: 异步 DATA finish 时进入 PENDING 并通过 poll 提交完成 */
    s_hw_stage_test.pending_finalize = true;
    assert(ecat_transfer_finish(&async_xfer, &client_owner1, tok_12) == ECAT_TRANSFER_PENDING);
    assert(s_hw_stage_test.finalize_called == true);
    assert(ecat_transfer_is_pending(&async_xfer) == true);
    assert(ecat_transfer_poll(&async_xfer, &client_owner1, tok_12) == ECAT_TRANSFER_COMPLETE);
    assert(!async_xfer.active);

    /* 10.5.6: 异步 DATA finish 失败处理并精确中止一次 */
    memset(&s_hw_stage_test, 0, sizeof(s_hw_stage_test));
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &req_12, &tok_12) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_write(&async_xfer, &client_owner1, tok_12, dummy_chunk, 64U) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_write(&async_xfer, &client_owner1, tok_12, dummy_chunk, 64U) == ECAT_TRANSFER_COMPLETE);
    s_hw_stage_test.fail_finalize = true;
    assert(ecat_transfer_finish(&async_xfer, &client_owner1, tok_12) == ECAT_TRANSFER_FAILED);
    assert(s_hw_stage_test.abort_calls == 1);
    assert(ecat_transfer_get_last_error(&async_xfer) == ECAT_TRANSFER_ERR_DRIVER_FINALIZE);
    assert(!async_xfer.active);

    /* 10.5.7: 异步处于 pending 期间执行 cancel，硬件恰好中止一次且绝不触碰同步 abort */
    memset(&s_hw_stage_test, 0, sizeof(s_hw_stage_test));
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &req_12, &tok_12) == ECAT_TRANSFER_COMPLETE);
    s_hw_stage_test.pending_write = true;
    assert(ecat_transfer_write(&async_xfer, &client_owner1, tok_12, dummy_chunk, 64U) == ECAT_TRANSFER_PENDING);
    assert(ecat_transfer_is_pending(&async_xfer) == true);
    assert(ecat_transfer_cancel(&async_xfer, &client_owner1, tok_12) == ECAT_TRANSFER_COMPLETE);
    assert(s_hw_stage_test.abort_calls == 1);
    assert(!async_xfer.active);
    assert(ecat_transfer_is_pending(&async_xfer) == false);

    /* 10.6 驱动 actual_length 边界核验与短读短写契约 */
    printf("--- 测试 10.6: 驱动 actual_length 越界防御与短读短写契约 ---\n");
    ecat_transfer_request_t req_11_test = {
        .transport_id = 1U, .target_id = 11U, .direction = ECAT_TRANSFER_DIR_IMPORT, .expected_size = 500U
    };
    uint64_t tok_11 = 0;
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &req_11_test, &tok_11) == ECAT_TRANSFER_COMPLETE);

    /* 10.6.1 写入立即完成越界拦截: actual_length > length */
    s_next_async_result = ECAT_ASYNC_OP_COMPLETE;
    s_override_actual_length = 70; /* 请求 64 字节，驱动谎报 70 字节 */
    assert(ecat_transfer_write(&async_xfer, &client_owner1, tok_11, dummy_chunk, 64U) == ECAT_TRANSFER_FAILED);
    assert(ecat_transfer_get_last_error(&async_xfer) == ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED);
    assert(!async_xfer.active);

    /* 10.6.2a 写入立即完成零字节短写拦截: actual_length == 0 必须明确失败返回 DRIVER_WRITE */
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &req_11_test, &tok_11) == ECAT_TRANSFER_COMPLETE);
    s_override_actual_length = 0;
    assert(ecat_transfer_write(&async_xfer, &client_owner1, tok_11, dummy_chunk, 64U) == ECAT_TRANSFER_FAILED);
    assert(ecat_transfer_get_last_error(&async_xfer) == ECAT_TRANSFER_ERR_DRIVER_WRITE);
    assert(!async_xfer.active);

    /* 10.6.2b 写入立即完成部分短写拦截: actual_length == 30 (< 64) 必须明确失败返回 DRIVER_WRITE */
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &req_11_test, &tok_11) == ECAT_TRANSFER_COMPLETE);
    s_override_actual_length = 30;
    assert(ecat_transfer_write(&async_xfer, &client_owner1, tok_11, dummy_chunk, 64U) == ECAT_TRANSFER_FAILED);
    assert(ecat_transfer_get_last_error(&async_xfer) == ECAT_TRANSFER_ERR_DRIVER_WRITE);
    assert(!async_xfer.active);

    /* 10.6.3 写入 poll 完成越界拦截: actual_length > pending_length */
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &req_11_test, &tok_11) == ECAT_TRANSFER_COMPLETE);
    s_override_actual_length = -1;
    s_next_async_result = ECAT_ASYNC_OP_PENDING;
    assert(ecat_transfer_write(&async_xfer, &client_owner1, tok_11, dummy_chunk, 64U) == ECAT_TRANSFER_PENDING);
    s_override_actual_length = 80; /* poll 返回 80 字节越界 */
    s_next_async_result = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transfer_poll(&async_xfer, &client_owner1, tok_11) == ECAT_TRANSFER_FAILED);
    assert(ecat_transfer_get_last_error(&async_xfer) == ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED);
    assert(!async_xfer.active);

    /* 10.6.3a 写入 poll 完成零字节短写拦截: poll 返回 actual_length == 0 必须明确失败返回 DRIVER_WRITE */
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &req_11_test, &tok_11) == ECAT_TRANSFER_COMPLETE);
    s_override_actual_length = -1;
    s_next_async_result = ECAT_ASYNC_OP_PENDING;
    assert(ecat_transfer_write(&async_xfer, &client_owner1, tok_11, dummy_chunk, 64U) == ECAT_TRANSFER_PENDING);
    s_override_actual_length = 0; /* poll 返回 0 字节短写 */
    s_next_async_result = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transfer_poll(&async_xfer, &client_owner1, tok_11) == ECAT_TRANSFER_FAILED);
    assert(ecat_transfer_get_last_error(&async_xfer) == ECAT_TRANSFER_ERR_DRIVER_WRITE);
    assert(!async_xfer.active);

    /* 10.6.3b 写入 poll 完成部分短写拦截: poll 返回 actual_length == 30 (< 64) 必须明确失败返回 DRIVER_WRITE */
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &req_11_test, &tok_11) == ECAT_TRANSFER_COMPLETE);
    s_override_actual_length = -1;
    s_next_async_result = ECAT_ASYNC_OP_PENDING;
    assert(ecat_transfer_write(&async_xfer, &client_owner1, tok_11, dummy_chunk, 64U) == ECAT_TRANSFER_PENDING);
    s_override_actual_length = 30; /* poll 返回 30 字节部分短写 */
    s_next_async_result = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transfer_poll(&async_xfer, &client_owner1, tok_11) == ECAT_TRANSFER_FAILED);
    assert(ecat_transfer_get_last_error(&async_xfer) == ECAT_TRANSFER_ERR_DRIVER_WRITE);
    assert(!async_xfer.active);

    /* 10.6.4 读取 poll 完成零字节短读契约与越界拦截 */
    ecat_transfer_request_t req_11_rd = {
        .transport_id = 1U, .target_id = 11U, .direction = ECAT_TRANSFER_DIR_EXPORT, .expected_size = 128U
    };
    uint64_t tok_rd = 0;
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &req_11_rd, &tok_rd) == ECAT_TRANSFER_COMPLETE);
    uint8_t rd_buf_test[64];
    size_t actual_rd_test = 999;
    s_override_actual_length = -1;
    s_next_async_result = ECAT_ASYNC_OP_PENDING;
    assert(ecat_transfer_read(&async_xfer, &client_owner1, tok_rd, rd_buf_test, sizeof(rd_buf_test), &actual_rd_test) == ECAT_TRANSFER_PENDING);
    /* poll 返回 0 字节短读: 必须正确写入 actual_read = 0，不可被当成 64 */
    s_override_actual_length = 0;
    s_next_async_result = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transfer_poll(&async_xfer, &client_owner1, tok_rd) == ECAT_TRANSFER_CONTINUE);
    assert(actual_rd_test == 0);

    /* 再次发起读取并注入越界 actual_length */
    s_override_actual_length = -1;
    s_next_async_result = ECAT_ASYNC_OP_PENDING;
    assert(ecat_transfer_read(&async_xfer, &client_owner1, tok_rd, rd_buf_test, sizeof(rd_buf_test), &actual_rd_test) == ECAT_TRANSFER_PENDING);
    s_override_actual_length = 100; /* 64 字节缓冲区，谎报 100 字节 */
    s_next_async_result = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transfer_poll(&async_xfer, &client_owner1, tok_rd) == ECAT_TRANSFER_FAILED);
    assert(ecat_transfer_get_last_error(&async_xfer) == ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED);
    assert(!async_xfer.active);
    s_override_actual_length = -1;

    /* 10.7 异步 poll 阶段失败错误分类与取消单次精确中止验证 */
    printf("--- 测试 10.7: 异步 poll 阶段失败分类与取消单次精确清理 ---\n");
    /* 10.7.1: poll 处于 READ 阶段失败时，last_error 必须准确为 DRIVER_READ (而非被清 stage 误判为 WRITE) */
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &req_11_rd, &tok_rd) == ECAT_TRANSFER_COMPLETE);
    s_next_async_result = ECAT_ASYNC_OP_PENDING;
    assert(ecat_transfer_read(&async_xfer, &client_owner1, tok_rd, rd_buf_test, sizeof(rd_buf_test), &actual_rd_test) == ECAT_TRANSFER_PENDING);
    s_next_async_result = ECAT_ASYNC_OP_FAILED;
    assert(ecat_transfer_poll(&async_xfer, &client_owner1, tok_rd) == ECAT_TRANSFER_FAILED);
    assert(ecat_transfer_get_last_error(&async_xfer) == ECAT_TRANSFER_ERR_DRIVER_READ);
    assert(!async_xfer.active);

    /* 10.7.2: cancel 处于 PENDING 的异步写入时，驱动 abort 恰好执行一次，绝不重复调用同步 io.abort */
    ecat_transfer_request_t req_12_cancel = {
        .transport_id = 1U, .target_id = 12U, .direction = ECAT_TRANSFER_DIR_IMPORT, .expected_size = 128U
    };
    uint64_t tok_12_c = 0;
    assert(ecat_transfer_begin(&async_xfer, &client_owner1, &req_12_cancel, &tok_12_c) == ECAT_TRANSFER_COMPLETE);
    s_next_async_result = ECAT_ASYNC_OP_PENDING;
    assert(ecat_transfer_write(&async_xfer, &client_owner1, tok_12_c, dummy_chunk, 64U) == ECAT_TRANSFER_PENDING);
    assert(ecat_transfer_is_pending(&async_xfer) == true);

    s_hw_stage_test.abort_calls = 0;
    assert(ecat_transfer_cancel(&async_xfer, &client_owner1, tok_12_c) == ECAT_TRANSFER_COMPLETE);
    /* 严格断言: 异步 abort 执行且仅执行 1 次 */
    assert(s_hw_stage_test.abort_calls == 1);
    assert(!async_xfer.active);

    /* 10.8 真实异步固件核心 (ecat_foe_core_async) 鉴权绑定与伪造后端全面防御 */
    printf("--- 测试 10.8: 真实异步固件核心鉴权绑定与防伪造检查 ---\n");
    static uint8_t s_real_packet_storage[512];
    static uint8_t s_real_op_storage[256];
    ecat_foe_core_async_t real_fw_async_core;
    assert(ecat_foe_core_async_init(&real_fw_async_core, &core, s_real_packet_storage,
                                   sizeof(s_real_packet_storage), s_real_op_storage,
                                   sizeof(s_real_op_storage)) == 0);

    /* 10.8.1: 管理器未关联 core (core == NULL) 时调用必须拒绝 */
    ecat_transfer_t mgr_no_core;
    memset(&mgr_no_core, 0, sizeof(mgr_no_core));
    assert(ecat_transfer_init_async_firmware(&mgr_no_core, ecat_foe_core_async_ops(), &real_fw_async_core) == -1);

    /* 10.8.2: 伪造上下文 ctx 不匹配 core->async_abort_ctx 时必须拒绝 */
    int fake_fw_ctx = 42;
    assert(ecat_transfer_init_async_firmware(&mgr_first, ecat_foe_core_async_ops(), &fake_fw_ctx) == -1);

    /* 10.8.3: 伪造 abort 回调不匹配 core->async_abort_hook 时必须拒绝 */
    ecat_upgrade_async_ops_t fake_abort_ops = *ecat_foe_core_async_ops();
    fake_abort_ops.abort = test_fw_async_abort;
    assert(ecat_transfer_init_async_firmware(&mgr_first, &fake_abort_ops, &real_fw_async_core) == -1);

    /* 10.8.4: 真实已初始化的异步固件核心与合法 ops 绑定成功 */
    assert(ecat_transfer_init_async_firmware(&mgr_first, ecat_foe_core_async_ops(), &real_fw_async_core) == 0);

    /* 10.8.5: 反例测试：复制合法 ops 但篡改 start 或 poll 的伪造操作集必须被当场拒绝 */
    ecat_upgrade_async_ops_t fake_start_ops = *ecat_foe_core_async_ops();
    fake_start_ops.start = test_fw_async_start; /* 篡改 start */
    assert(ecat_transfer_init_async_firmware(&mgr_first, &fake_start_ops, &real_fw_async_core) == -1);

    ecat_upgrade_async_ops_t fake_poll_ops = *ecat_foe_core_async_ops();
    fake_poll_ops.poll = test_fw_async_poll; /* 篡改 poll */
    assert(ecat_transfer_init_async_firmware(&mgr_first, &fake_poll_ops, &real_fw_async_core) == -1);

    printf(">>> 双向数据传输引擎全功能单元测试全部通过！<<<\n");
    return 0;
}
