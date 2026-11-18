/**
 * @file test_transport_endpoint.c
 * @brief 自定义与通用传输端点层单元测试
 */

#include "ecat_transport_endpoint.h"
#include "ecat_foe_efw.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

int port_get_version_floor(uint16_t id, uint32_t *v)
{
    (void)id;
    if (v) *v = 0;
    return 0;
}

#define TEST_FLASH_SIZE 2048U
static uint8_t s_flash[TEST_FLASH_SIZE];

static int mock_init(void *ctx) { (void)ctx; return 0; }
static bool s_read_opened;
static bool s_size_requires_open;
static int mock_read_open(void *ctx) { (void)ctx; s_read_opened = true; return 0; }
static void mock_read_close(void *ctx) { (void)ctx; s_read_opened = false; }
static int mock_invalidate(void *ctx) { (void)ctx; return 0; }
static int mock_erase_to(uint32_t need_end, uint32_t *erased_end, void *ctx)
{
    (void)ctx;
    if (erased_end) *erased_end = need_end;
    return 0;
}
static int mock_write(uint32_t offset, const uint8_t *data, uint32_t len, void *ctx)
{
    (void)ctx;
    if (offset + len > TEST_FLASH_SIZE) return -1;
    memcpy(&s_flash[offset], data, len);
    return 0;
}
static int mock_read(uint32_t offset, uint8_t *buf, uint32_t len, void *ctx)
{
    (void)ctx;
    if (offset + len > TEST_FLASH_SIZE) return -1;
    memcpy(buf, &s_flash[offset], len);
    return 0;
}
static int mock_finalize(void *ctx) { (void)ctx; return 0; }
static void mock_abort(void *ctx) { (void)ctx; }
static uint32_t s_mock_size_override = TEST_FLASH_SIZE;
static uint32_t mock_get_size(void *ctx) { (void)ctx; return s_size_requires_open && !s_read_opened ? 0U : s_mock_size_override; }

/* 回调跟踪 */
static bool s_cb_started = false;
static uint32_t s_cb_progress_last = 0;
static bool s_cb_completed = false;
static bool s_cb_aborted = false;
static uint32_t s_cb_completed_crc = 0;
static int s_cb_completed_count = 0;
static int s_cb_aborted_count = 0;

static void on_start(void *ctx, ecat_transfer_dir_t dir, uint16_t target_id)
{
    (void)ctx; (void)dir; (void)target_id;
    s_cb_started = true;
}

static void on_progress(void *ctx, uint32_t cur, uint32_t total)
{
    (void)ctx; (void)total;
    s_cb_progress_last = cur;
}

static void on_complete(void *ctx, uint32_t total, uint32_t crc)
{
    (void)ctx; (void)total;
    s_cb_completed = true;
    s_cb_completed_crc = crc;
    s_cb_completed_count++;
}

static void on_abort(void *ctx, ecat_transfer_err_t err)
{
    (void)ctx; (void)err;
    s_cb_aborted = true;
    s_cb_aborted_count++;
}

static void reset_cb_flags(void)
{
    s_cb_started = false;
    s_cb_progress_last = 0;
    s_cb_completed = false;
    s_cb_aborted = false;
    s_cb_completed_crc = 0;
    s_cb_completed_count = 0;
    s_cb_aborted_count = 0;
}

/* Mock 异步端点数据驱动 */
static ecat_async_op_result_t s_ep_async_next_res = ECAT_ASYNC_OP_COMPLETE;
static size_t s_ep_mock_override_actual = 0;
static bool s_ep_mock_use_override_actual = false;

static ecat_async_op_result_t ep_mock_async_start(void *ctx, ecat_async_data_op_t *op)
{
    (void)ctx;
    if (op != NULL) {
        if (op->kind == ECAT_ASYNC_DATA_OP_READ_OPEN && s_ep_async_next_res == ECAT_ASYNC_OP_COMPLETE) s_read_opened = true;
        if (s_ep_mock_use_override_actual) {
            op->actual_length = s_ep_mock_override_actual;
        } else {
            op->actual_length = op->length;
        }
        if (op->kind == ECAT_ASYNC_DATA_OP_ERASE_TO) {
            op->erased_end = op->need_end;
        }
    }
    return s_ep_async_next_res;
}
static ecat_async_op_result_t ep_mock_async_poll(void *ctx, ecat_async_data_op_t *op)
{
    (void)ctx;
    if (op != NULL) {
        if (op->kind == ECAT_ASYNC_DATA_OP_READ_OPEN && s_ep_async_next_res == ECAT_ASYNC_OP_COMPLETE) s_read_opened = true;
        if (s_ep_mock_use_override_actual) {
            op->actual_length = s_ep_mock_override_actual;
        } else {
            op->actual_length = op->length;
        }
        if (op->kind == ECAT_ASYNC_DATA_OP_ERASE_TO) {
            op->erased_end = op->need_end;
        }
    }
    return s_ep_async_next_res;
}
static int s_ep_mock_abort_count = 0;
static void ep_mock_async_abort(void *ctx)
{
    (void)ctx;
    s_ep_mock_abort_count++;
}

int main(void)
{
    printf("=== 开始运行传输端点接口与自定义通道单元测试 ===\n");

    ecat_target_registry_t reg;
    assert(ecat_target_registry_init(&reg) == 0);

    /* 注册 Target 1: RW 数据区 */
    ecat_target_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.target_id = 1U;
    strncpy(desc.name, "shared_storage", sizeof(desc.name));
    desc.type = ECAT_TARGET_TYPE_DATA;
    desc.permissions = ECAT_PERM_RW;
    desc.max_size = TEST_FLASH_SIZE;
    desc.enabled = true;
    desc.driver.read_open = mock_read_open;
    desc.driver.read_close = mock_read_close;
    desc.driver.io.init = mock_init;
    desc.driver.io.invalidate = mock_invalidate;
    desc.driver.io.erase_to = mock_erase_to;
    desc.driver.io.write = mock_write;
    desc.driver.io.read = mock_read;
    desc.driver.io.finalize = mock_finalize;
    desc.driver.io.abort = mock_abort;
    desc.driver.get_data_size = mock_get_size;
    assert(ecat_target_registry_add(&reg, &desc) == 0);

    ecat_transfer_t mgr;
    assert(ecat_transfer_init(&mgr, &reg, NULL) == 0);

    /* 创建两个自定义传输通道端点: UART (ID=101) 和 CAN (ID=102) */
    ecat_transport_endpoint_t ep_uart;
    ecat_transport_endpoint_t ep_can;

    assert(ecat_transport_endpoint_init(NULL, 101, "custom_uart", &mgr, NULL) == -1);
    assert(ecat_transport_endpoint_init(&ep_uart, 101, "custom_uart", &mgr, NULL) == 0);
    assert(ecat_transport_endpoint_init(&ep_can, 102, "custom_can", &mgr, NULL) == 0);
    /* 超过或等于 16 字节且无 NUL 终止符的端点名称必须严格拒绝 (-1) */
    assert(ecat_transport_endpoint_init(&ep_uart, 103, "0123456789abcdef", &mgr, NULL) == -1);

    ecat_transport_callbacks_t cbs = {
        .on_session_start = on_start,
        .on_progress = on_progress,
        .on_session_complete = on_complete,
        .on_session_abort = on_abort
    };
    ecat_transport_endpoint_set_callbacks(&ep_uart, &cbs);

    s_size_requires_open = true;
    uint32_t opened_size = 0;
    assert(ecat_transport_endpoint_begin_export(&ep_uart, 1U, &opened_size) == ECAT_TRANSFER_COMPLETE);
    assert(opened_size == TEST_FLASH_SIZE);
    uint8_t sync_export_buf[TEST_FLASH_SIZE];
    size_t sync_read = 0U;
    uint64_t sync_export_token = ep_uart.current_token;
    assert(ecat_transport_endpoint_read_chunk(&ep_uart, sync_export_buf, 1U, &sync_read) == ECAT_TRANSFER_CONTINUE);
    uint32_t replacement_size = 123U;
    assert(ecat_transport_endpoint_begin_export(&ep_uart, 0U, &replacement_size) == ECAT_TRANSFER_BUSY);
    assert(replacement_size == 123U);
    assert(ep_uart.current_token == sync_export_token);
    assert(ecat_transfer_get_bytes_transferred(&mgr) == 1U);
    assert(ecat_transport_endpoint_finish_export(&ep_uart) == ECAT_TRANSFER_INVALID);
    assert(ep_uart.current_token == sync_export_token);
    assert(s_read_opened && !s_cb_completed && s_cb_aborted_count == 0);
    assert(ecat_transport_endpoint_read_chunk(&ep_uart, sync_export_buf, sizeof(sync_export_buf), &sync_read)
           == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_finish_export(&ep_uart) == ECAT_TRANSFER_COMPLETE);
    s_size_requires_open = false;

    /* ---------------------------------------------------------------------- */
    /* 1. UART 端点数据流写入导入测试                                        */
    /* ---------------------------------------------------------------------- */
    reset_cb_flags();
    uint8_t payload[512];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 3 + 1);
    }
    uint32_t expected_crc = ecat_foe_crc32_calc(payload, sizeof(payload));

    assert(!ecat_transport_endpoint_is_active(&ep_uart));
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 1U, sizeof(payload), 0) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_is_active(&ep_uart));
    assert(s_cb_started);

    /* 同步目标导入: 在 begin 之后、首次 write 之前空闲轮询，必须返回 INVALID 且保留会话 */
    assert(ecat_transport_endpoint_poll(&ep_uart) == ECAT_TRANSFER_INVALID);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);
    assert(s_cb_aborted_count == 0);

    /* CAN 端点并发争抢拦截 */
    assert(ecat_transport_endpoint_begin_export(&ep_can, 1U, NULL) == ECAT_TRANSFER_BUSY);

    /* 分两包写入，并在每包写完后空闲轮询，验证不触发 abort */
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, payload, 200) == ECAT_TRANSFER_COMPLETE);
    assert(s_cb_progress_last == 200);
    assert(ecat_transport_endpoint_poll(&ep_uart) == ECAT_TRANSFER_INVALID);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);
    assert(s_cb_aborted_count == 0);

    assert(ecat_transport_endpoint_write_chunk(&ep_uart, payload + 200, 312) == ECAT_TRANSFER_COMPLETE);
    assert(s_cb_progress_last == 512);
    assert(ecat_transport_endpoint_poll(&ep_uart) == ECAT_TRANSFER_INVALID);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);
    assert(s_cb_aborted_count == 0);

    /* 提交会话 */
    assert(ecat_transport_endpoint_finish_import(&ep_uart) == ECAT_TRANSFER_COMPLETE);
    assert(s_cb_completed && s_cb_completed_crc == expected_crc);
    assert(!ecat_transport_endpoint_is_active(&ep_uart));
    assert(memcmp(s_flash, payload, sizeof(payload)) == 0);

    /* ---------------------------------------------------------------------- */
    /* 2. CAN 端点数据回读导出测试                                            */
    /* ---------------------------------------------------------------------- */
    ecat_transport_endpoint_set_callbacks(&ep_can, &cbs);
    reset_cb_flags();

    uint32_t export_size = 0;
    assert(ecat_transport_endpoint_begin_export(&ep_can, 1U, &export_size) == ECAT_TRANSFER_COMPLETE);
    assert(export_size == TEST_FLASH_SIZE);
    assert(ecat_transport_endpoint_is_active(&ep_can));
    assert(s_cb_started);

    /* 同步目标导出: 在 begin 之后、首次 read 之前空闲轮询，必须返回 INVALID 且保留会话 */
    assert(ecat_transport_endpoint_poll(&ep_can) == ECAT_TRANSFER_INVALID);
    assert(ecat_transport_endpoint_is_active(&ep_can) == true);
    assert(s_cb_aborted_count == 0);

    uint8_t rx_buf[TEST_FLASH_SIZE];
    size_t total_rx = 0;
    while (total_rx < export_size) {
        uint8_t temp[128];
        size_t actual = 0;
        ecat_transfer_result_t r = ecat_transport_endpoint_read_chunk(
            &ep_can, temp, sizeof(temp), &actual);
        assert(actual > 0);
        memcpy(&rx_buf[total_rx], temp, actual);
        total_rx += actual;
        if (total_rx == export_size) {
            assert(r == ECAT_TRANSFER_COMPLETE);
        } else {
            assert(r == ECAT_TRANSFER_CONTINUE);
        }
        /* 同步 read 刚完成后空闲轮询，确保不会被错误 abort */
        assert(ecat_transport_endpoint_poll(&ep_can) == ECAT_TRANSFER_INVALID);
        assert(s_cb_aborted_count == 0);
    }

    assert(ecat_transport_endpoint_finish_export(&ep_can) == ECAT_TRANSFER_COMPLETE);
    assert(s_cb_completed);
    assert(!ecat_transport_endpoint_is_active(&ep_can));
    assert(memcmp(rx_buf, s_flash, TEST_FLASH_SIZE) == 0);

    /* ---------------------------------------------------------------------- */
    /* 3. 中止与异常清理测试                                                  */
    /* ---------------------------------------------------------------------- */
    reset_cb_flags();
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 1U, 100, 0) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_is_active(&ep_uart));
    ecat_transport_endpoint_abort(&ep_uart);
    assert(!ecat_transport_endpoint_is_active(&ep_uart));
    assert(s_cb_aborted);

    /* ---------------------------------------------------------------------- */
    /* 4. Issue 5: 可恢复/终止错误契约与会话清理验证 (Write & Read 均测试)    */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试 Issue 5: 端点错误后的会话生命周期与释放 ---\n");
    /* 4.1 写入路径空指针错误后调用 abort 必须释放底层传输会话 */
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 1U, 16, 0) == ECAT_TRANSFER_COMPLETE);
    int wr_err = ecat_transport_endpoint_write_chunk(&ep_uart, NULL, 1);
    assert(wr_err == ECAT_TRANSFER_INVALID);
    /* 此时会话未被销毁，允许调用方 abort 释放会话 */
    ecat_transport_endpoint_abort(&ep_uart);
    assert(ep_uart.current_token == 0U);
    /* 验证底层传输管理器会话已被释放，其它通道可正常 begin */
    assert(ecat_transport_endpoint_begin_import(&ep_can, 1U, 16, 0) == ECAT_TRANSFER_COMPLETE);
    ecat_transport_endpoint_abort(&ep_can);

    /* 4.2 写入路径可重试契约: 参数错误后更正参数可继续写入 */
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 1U, 16, 0) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, NULL, 1) == ECAT_TRANSFER_INVALID);
    /* 重试写入合法数据，重复 begin 保留已经写入的数据与会话。 */
    uint8_t retry_buf[16] = {0xAA};
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, retry_buf, 8U) == ECAT_TRANSFER_COMPLETE);
    uint64_t import_token = ep_uart.current_token;
    int abort_count = s_cb_aborted_count;
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 0U, 1U, 0U) == ECAT_TRANSFER_BUSY);
    assert(ecat_transport_endpoint_begin_export(&ep_uart, 1U, NULL) == ECAT_TRANSFER_BUSY);
    assert(ep_uart.current_token == import_token);
    assert(ecat_transfer_get_bytes_transferred(&mgr) == 8U);
    assert(s_cb_aborted_count == abort_count);
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, retry_buf + 8U, 8U) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_finish_import(&ep_uart) == ECAT_TRANSFER_COMPLETE);

    /* 4.3 回读导出路径空指针错误后调用 abort 必须释放底层传输会话 */
    uint32_t rd_sz = 0;
    assert(ecat_transport_endpoint_begin_export(&ep_uart, 1U, &rd_sz) == ECAT_TRANSFER_COMPLETE);
    size_t actual_rd = 0;
    int rd_err = ecat_transport_endpoint_read_chunk(&ep_uart, NULL, 16, &actual_rd);
    assert(rd_err == ECAT_TRANSFER_INVALID);
    ecat_transport_endpoint_abort(&ep_uart);
    assert(ep_uart.current_token == 0U);
    /* 验证底层会话已释放，ep_can 可正常发起会话 */
    assert(ecat_transport_endpoint_begin_export(&ep_can, 1U, &rd_sz) == ECAT_TRANSFER_COMPLETE);
    ecat_transport_endpoint_abort(&ep_can);

    /* 4.4 回读导出路径可重试契约: 参数错误后更正参数可继续回读 */
    assert(ecat_transport_endpoint_begin_export(&ep_uart, 1U, &rd_sz) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_read_chunk(&ep_uart, NULL, 16, &actual_rd) == ECAT_TRANSFER_INVALID);
    uint8_t rd_buf[16];
    ecat_transfer_result_t retry_rd = ecat_transport_endpoint_read_chunk(
        &ep_uart, rd_buf, sizeof(rd_buf), &actual_rd);
    assert(retry_rd == ECAT_TRANSFER_CONTINUE || retry_rd == ECAT_TRANSFER_COMPLETE);
    ecat_transport_endpoint_abort(&ep_uart);

    /* ---------------------------------------------------------------------- */
    /* 5. 异步传输端点推进、轮询与单次事件通知保障验证 (Endpoint Async Testing) */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试端点异步推进、非阻塞轮询与单次完成通知保障 ---\n");
    ecat_async_data_ops_t ep_async_ops = {
        .start = ep_mock_async_start,
        .poll = ep_mock_async_poll,
        .abort = ep_mock_async_abort
    };
    ecat_target_desc_t async_target_desc;
    memset(&async_target_desc, 0, sizeof(async_target_desc));
    async_target_desc.target_id = 10U;
    strncpy(async_target_desc.name, "ep_async", sizeof(async_target_desc.name));
    async_target_desc.type = ECAT_TARGET_TYPE_DATA;
    async_target_desc.permissions = ECAT_PERM_RW;
    async_target_desc.max_size = 1024U;
    async_target_desc.enabled = true;
    async_target_desc.asynchronous = true;
    async_target_desc.driver.async_ops = &ep_async_ops;
    async_target_desc.driver.get_data_size = mock_get_size;
    assert(ecat_target_registry_add(&reg, &async_target_desc) == 0);

    /* 绑定暂存区 */
    uint8_t ep_staging[256];
    assert(ecat_transfer_set_staging_buffer(&mgr, ep_staging, sizeof(ep_staging)) == 0);

    reset_cb_flags();
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 10U, 64, 0) == ECAT_TRANSFER_COMPLETE);
    assert(s_cb_started == true);
    assert(ecat_transport_endpoint_is_active(&ep_uart));

    /* write 触发异步 PENDING，端点必须保持 IMPORTING 状态，不误调 abort 回调 */
    s_ep_async_next_res = ECAT_ASYNC_OP_PENDING;
    uint8_t ep_chunk[64];
    memset(ep_chunk, 0xCC, sizeof(ep_chunk));
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, ep_chunk, sizeof(ep_chunk)) == ECAT_TRANSFER_PENDING);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == true);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);
    assert(s_cb_aborted_count == 0);

    /* 在 PENDING 期间重复调用 write_chunk: 必须返回 BUSY，且保留会话、token与激活状态 */
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, ep_chunk, sizeof(ep_chunk)) == ECAT_TRANSFER_BUSY);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == true);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);
    assert(s_cb_aborted_count == 0);
    assert(ep_uart.current_token != 0U);

    /* 端点 poll 推进: 驱动保持 PENDING 时返回 PENDING */
    assert(ecat_transport_endpoint_poll(&ep_uart) == ECAT_TRANSFER_PENDING);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == true);

    /* 驱动完成时 poll 推进返回 COMPLETE，触发 on_progress */
    s_ep_async_next_res = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transport_endpoint_poll(&ep_uart) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == false);
    assert(s_cb_progress_last == 64U);

    /* 异步 write 刚完成后空闲轮询，必须返回 INVALID 且保留会话与令牌，绝不触发 abort */
    assert(ecat_transport_endpoint_poll(&ep_uart) == ECAT_TRANSFER_INVALID);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);
    assert(s_cb_aborted_count == 0);

    /* 异步 finish_import: 返回 PENDING */
    s_ep_async_next_res = ECAT_ASYNC_OP_PENDING;
    assert(ecat_transport_endpoint_finish_import(&ep_uart) == ECAT_TRANSFER_PENDING);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == true);

    /* 在 finish PENDING 期间重复调用 finish: 必须返回 BUSY 且保留会话 */
    assert(ecat_transport_endpoint_finish_import(&ep_uart) == ECAT_TRANSFER_BUSY);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == true);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);
    assert(s_cb_aborted_count == 0);
    assert(ep_uart.current_token != 0U);

    /* poll 推进 finish 终结提交 */
    s_ep_async_next_res = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transport_endpoint_poll(&ep_uart) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == false);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == false);

    /* 关键断言: 完成通知回调有且仅有触发 1 次 */
    assert(s_cb_completed_count == 1);
    assert(s_cb_aborted_count == 0);

    /* 重复调用 poll 必须返回 INVALID，且绝不重复触发完成通知 */
    assert(ecat_transport_endpoint_poll(&ep_uart) == ECAT_TRANSFER_INVALID);
    assert(s_cb_completed_count == 1);

    /* --- 测试 Issue 9: 异步 begin (导入/导出) 处于 PENDING 时的生命周期、防租约泄漏与并发拦截 --- */
    printf("--- 测试 Issue 9: 异步 begin PENDING 生命周期、事件时序与防租约泄漏 ---\n");

    /* 9.1 begin_import 处于 PENDING，保存 token，保持 IMPORTING，延迟触发 on_session_start */
    reset_cb_flags();
    s_ep_async_next_res = ECAT_ASYNC_OP_PENDING;
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 10U, 64U, 0) == ECAT_TRANSFER_PENDING);
    assert(ep_uart.current_token != 0U);
    assert(ep_uart.begin_pending == true);
    assert(ep_uart.state == ECAT_TRANSPORT_STATE_IMPORTING);
    assert(s_cb_started == false); /* 未真正就绪，绝不提前触发启动回调 */
    assert(s_cb_aborted == false);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == true);

    /* 9.2 在 begin_pending 期间调用分块写入与终结: 必须返回 BUSY 拦截，保留会话与令牌 */
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, ep_chunk, 64U) == ECAT_TRANSFER_BUSY);
    assert(ecat_transport_endpoint_finish_import(&ep_uart) == ECAT_TRANSFER_BUSY);
    assert(ep_uart.current_token != 0U);
    assert(ep_uart.begin_pending == true);

    /* 9.3 poll 推进直到就绪: 单次派发 on_session_start */
    assert(ecat_transport_endpoint_poll(&ep_uart) == ECAT_TRANSFER_PENDING);
    assert(s_cb_started == false);
    s_ep_async_next_res = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transport_endpoint_poll(&ep_uart) == ECAT_TRANSFER_COMPLETE);
    assert(ep_uart.begin_pending == false);
    assert(s_cb_started == true);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);

    /* [复现 Codex 报告的问题 1]: 在 begin_import 成功后、首次 write 之前，
     * 活动会话存在但当前无 pending 操作，调用 poll 得到 INVALID，断言 is_active 必须依然为 true */
    assert(ecat_transport_endpoint_poll(&ep_uart) == ECAT_TRANSFER_INVALID);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);

    /* 写入并正常结束 */
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, ep_chunk, 64U) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_finish_import(&ep_uart) == ECAT_TRANSFER_COMPLETE);
    assert(!ecat_transport_endpoint_is_active(&ep_uart));

    /* 9.4 begin_import 处于 PENDING 时执行 abort: 必须安全取消会话并释放租约，杜绝泄漏 */
    reset_cb_flags();
    s_ep_async_next_res = ECAT_ASYNC_OP_PENDING;
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 10U, 128U, 0) == ECAT_TRANSFER_PENDING);
    assert(ep_uart.current_token != 0U);
    assert(ep_uart.begin_pending == true);

    /* 中途调用端点 abort */
    ecat_transport_endpoint_abort(&ep_uart);
    assert(ep_uart.current_token == 0U);
    assert(ep_uart.begin_pending == false);
    assert(ep_uart.state == ECAT_TRANSPORT_STATE_IDLE);
    assert(!ecat_transport_endpoint_is_active(&ep_uart));
    assert(s_cb_aborted == true);
    /* 关键验证: 底层 transfer 管理器会话已完全释放，允许新会话立即获取 */
    assert(mgr.active == false);
    s_ep_async_next_res = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 10U, 64U, 0) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, ep_chunk, 64U) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_finish_import(&ep_uart) == ECAT_TRANSFER_COMPLETE);

    /* 9.5 begin_export 处于 PENDING 生命周期与 poll 推进 */
    reset_cb_flags();
    s_read_opened = false;
    s_size_requires_open = true;
    s_ep_async_next_res = ECAT_ASYNC_OP_PENDING;
    uint32_t exp_total = 0;
    assert(ecat_transport_endpoint_begin_export(&ep_uart, 10U, &exp_total) == ECAT_TRANSFER_PENDING);
    assert(ep_uart.current_token != 0U);
    assert(ep_uart.begin_pending == true);
    assert(ep_uart.state == ECAT_TRANSPORT_STATE_EXPORTING);
    assert(s_cb_started == false);
    assert(exp_total == 0U);

    uint64_t pending_export_token = ep_uart.current_token;
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 1U, 16U, 0U) == ECAT_TRANSFER_BUSY);
    assert(ep_uart.current_token == pending_export_token);
    assert(ep_uart.begin_pending && ecat_transport_endpoint_is_pending(&ep_uart));
    assert(s_cb_aborted_count == 0);

    /* 在 begin_pending 期间调用 read_chunk / finish_export: 必须返回 BUSY */
    uint8_t exp_buf[64];
    size_t actual_exp = 0;
    assert(ecat_transport_endpoint_read_chunk(&ep_uart, exp_buf, sizeof(exp_buf), &actual_exp) == ECAT_TRANSFER_BUSY);
    assert(ecat_transport_endpoint_finish_export(&ep_uart) == ECAT_TRANSFER_BUSY);

    /* poll 推进就绪 */
    s_ep_async_next_res = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transport_endpoint_poll(&ep_uart) == ECAT_TRANSFER_COMPLETE);
    assert(ep_uart.begin_pending == false);
    assert(s_cb_started == true);
    assert(ecat_transport_endpoint_get_total_export_size(&ep_uart) == 1024U);
    s_size_requires_open = false;

    /* 正常读取并完成 */
    assert(ecat_transport_endpoint_read_chunk(&ep_uart, exp_buf, sizeof(exp_buf), &actual_exp) == ECAT_TRANSFER_CONTINUE);
    assert(actual_exp == sizeof(exp_buf));

    /* 异步 read 刚完成后空闲轮询，必须返回 INVALID 且保留会话与令牌，绝不触发 abort */
    assert(ecat_transport_endpoint_poll(&ep_uart) == ECAT_TRANSFER_INVALID);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);
    assert(s_cb_aborted_count == 0);

    uint64_t async_export_token = ep_uart.current_token;
    assert(ecat_transport_endpoint_finish_export(&ep_uart) == ECAT_TRANSFER_INVALID);
    assert(ep_uart.current_token == async_export_token);
    assert(s_read_opened && !s_cb_completed && s_cb_aborted_count == 0);
    ecat_transfer_result_t read_result;
    do {
        read_result = ecat_transport_endpoint_read_chunk(&ep_uart, exp_buf, sizeof(exp_buf), &actual_exp);
    } while (read_result == ECAT_TRANSFER_CONTINUE);
    assert(read_result == ECAT_TRANSFER_COMPLETE);

    assert(ecat_transport_endpoint_finish_export(&ep_uart) == ECAT_TRANSFER_COMPLETE);
    assert(!ecat_transport_endpoint_is_active(&ep_uart));

    /* ---------------------------------------------------------------------- */
    /* 10. 真正底层驱动故障时的错误清理与 abort 回调验证                      */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试端点底层驱动真实故障时的状态清理与 abort 派发 ---\n");
    reset_cb_flags();
    s_ep_async_next_res = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 10U, 128U, 0) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);

    /* 启动一个分块写入，底层返回 PENDING */
    s_ep_async_next_res = ECAT_ASYNC_OP_PENDING;
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, ep_chunk, 64U) == ECAT_TRANSFER_PENDING);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == true);

    /* 模拟底层驱动在 poll 推进期间发生真实致命错误 (FAILED) */
    s_ep_async_next_res = ECAT_ASYNC_OP_FAILED;
    assert(ecat_transport_endpoint_poll(&ep_uart) == ECAT_TRANSFER_FAILED);
    /* 关键断言: 真正驱动错误下必须完全清理会话、触发 abort 回调并释放令牌与 transfer 管理器 */
    assert(ecat_transport_endpoint_is_active(&ep_uart) == false);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == false);
    assert(ep_uart.current_token == 0U);
    assert(ep_uart.state == ECAT_TRANSPORT_STATE_ERROR);
    assert(s_cb_aborted == true);
    assert(s_cb_aborted_count == 1);
    assert(mgr.active == false);

    /* 验证错误后端点可重新开启新会话 */
    reset_cb_flags();
    s_ep_async_next_res = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 10U, 64U, 0) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, ep_chunk, 64U) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_finish_import(&ep_uart) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == false);

    /* ---------------------------------------------------------------------- */
    /* 11. 异步 read 缓冲区与指针生命周期、延时赋值与 abort 硬件停止验证         */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试异步 read 指针持有、延时赋值与 abort 硬件停止契约 ---\n");
    reset_cb_flags();
    s_ep_async_next_res = ECAT_ASYNC_OP_COMPLETE;
    uint32_t exp_total_sz = 0;
    assert(ecat_transport_endpoint_begin_export(&ep_uart, 10U, &exp_total_sz) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);

    /* 11.1 发起异步 read，驱动返回 PENDING */
    s_ep_async_next_res = ECAT_ASYNC_OP_PENDING;
    uint8_t async_read_buf[128];
    memset(async_read_buf, 0xEE, sizeof(async_read_buf));
    size_t delayed_actual_read = 0x5A5A; /* 初始化为金丝雀哨兵值 */

    assert(ecat_transport_endpoint_read_chunk(
        &ep_uart, async_read_buf, sizeof(async_read_buf), &delayed_actual_read) == ECAT_TRANSFER_PENDING);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == true);
    /* 契约断言: 在 pending 期间，actual_read 被赋初值 0，绝不提早赋予最终读取长度 */
    assert(delayed_actual_read == 0U);

    /* 11.2 poll 推进直到完成: 此时 *actual_read 被正式赋值为实际读取长度 */
    s_ep_async_next_res = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transport_endpoint_poll(&ep_uart) == ECAT_TRANSFER_CONTINUE);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == false);
    assert(delayed_actual_read == sizeof(async_read_buf));

    /* 11.3 再次发起异步 read 进入 PENDING，随后调用 abort 验证硬件停止契约 */
    s_ep_async_next_res = ECAT_ASYNC_OP_PENDING;
    delayed_actual_read = 0xA5A5;
    assert(ecat_transport_endpoint_read_chunk(
        &ep_uart, async_read_buf, sizeof(async_read_buf), &delayed_actual_read) == ECAT_TRANSFER_PENDING);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == true);
    assert(delayed_actual_read == 0U);

    int before_abort_count = s_ep_mock_abort_count;
    /* 调用端点 abort 中止会话 */
    ecat_transport_endpoint_abort(&ep_uart);
    /* 契约断言: 硬件驱动 abort 回调必须被同步调用，且会话与 pending 标记完全重置 */
    assert(s_ep_mock_abort_count == before_abort_count + 1);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == false);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == false);
    assert(mgr.active == false);
    assert(s_cb_aborted == true);
    /* *actual_read 保持中止时状态 (0)，不会在 abort 后被异步驱动残留改写 */
    assert(delayed_actual_read == 0U);

    /* ---------------------------------------------------------------------- */
    /* 12. 异步导出零进展 (actual_length == 0) 死循环拦截与安全失败验证        */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试异步导出零进展 (actual_length == 0) 死循环拦截与安全失败 ---\n");
    /* 12.1 路径 A: start 立即 COMPLETE 但 actual_length == 0 (死循环拦截) */
    reset_cb_flags();
    s_ep_async_next_res = ECAT_ASYNC_OP_COMPLETE;
    s_ep_mock_use_override_actual = false;
    uint32_t zero_prog_total = 0;
    assert(ecat_transport_endpoint_begin_export(&ep_uart, 10U, &zero_prog_total) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);
    assert(zero_prog_total == 1024U);

    s_ep_mock_use_override_actual = true;
    s_ep_mock_override_actual = 0;
    s_ep_async_next_res = ECAT_ASYNC_OP_COMPLETE;
    uint8_t zero_prog_buf[64];
    size_t zero_actual = 0x1234;

    int abort_before_a = s_ep_mock_abort_count;
    ecat_transfer_result_t zres_a = ecat_transport_endpoint_read_chunk(
        &ep_uart, zero_prog_buf, sizeof(zero_prog_buf), &zero_actual);
    /* 契约断言: 立即完成且 0 字节时必须返回 FAILED，杜绝死循环并触发 abort */
    assert(zres_a == ECAT_TRANSFER_FAILED);
    assert(ecat_transfer_get_last_error(&mgr) == ECAT_TRANSFER_ERR_DRIVER_READ);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == false);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == false);
    assert(ep_uart.current_token == 0U);
    assert(ep_uart.state == ECAT_TRANSPORT_STATE_ERROR);
    assert(mgr.active == false);
    assert(s_cb_aborted == true);
    assert(s_ep_mock_abort_count == abort_before_a + 1);

    /* 验证错误后会话锁完全释放，允许开启新会话 */
    reset_cb_flags();
    s_ep_mock_use_override_actual = false;
    assert(ecat_transport_endpoint_begin_export(&ep_uart, 10U, &zero_prog_total) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);

    /* 12.2 路径 B: start 返回 PENDING，后续 poll COMPLETE 但 actual_length == 0 */
    s_ep_async_next_res = ECAT_ASYNC_OP_PENDING;
    s_ep_mock_use_override_actual = false;
    size_t poll_zero_actual = 0x5678;
    assert(ecat_transport_endpoint_read_chunk(
        &ep_uart, zero_prog_buf, sizeof(zero_prog_buf), &poll_zero_actual) == ECAT_TRANSFER_PENDING);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == true);
    assert(poll_zero_actual == 0U);

    /* 模拟 poll 返回 COMPLETE 但驱动报告 0 字节 */
    s_ep_mock_use_override_actual = true;
    s_ep_mock_override_actual = 0;
    s_ep_async_next_res = ECAT_ASYNC_OP_COMPLETE;
    int abort_before_b = s_ep_mock_abort_count;

    ecat_transfer_result_t zres_b = ecat_transport_endpoint_poll(&ep_uart);
    /* 契约断言: poll 阶段推进返回 0 字节必须返回 FAILED 并清理会话 */
    assert(zres_b == ECAT_TRANSFER_FAILED);
    assert(ecat_transfer_get_last_error(&mgr) == ECAT_TRANSFER_ERR_DRIVER_READ);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == false);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == false);
    assert(ep_uart.current_token == 0U);
    assert(ep_uart.state == ECAT_TRANSPORT_STATE_ERROR);
    assert(mgr.active == false);
    assert(s_cb_aborted == true);
    assert(s_ep_mock_abort_count == abort_before_b + 1);

    /* 12.3 允许正数短读 (Short Read) 正常推进 */
    reset_cb_flags();
    s_ep_mock_use_override_actual = false;
    assert(ecat_transport_endpoint_begin_export(&ep_uart, 10U, &zero_prog_total) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);

    s_ep_mock_use_override_actual = true;
    s_ep_mock_override_actual = 16; /* 请求 64 字节，实际返回 16 字节短读 */
    s_ep_async_next_res = ECAT_ASYNC_OP_COMPLETE;
    size_t short_actual = 0;
    assert(ecat_transport_endpoint_read_chunk(
        &ep_uart, zero_prog_buf, sizeof(zero_prog_buf), &short_actual) == ECAT_TRANSFER_CONTINUE);
    assert(short_actual == 16U);
    assert(ecat_transfer_get_bytes_transferred(&mgr) == 16U);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);

    /* 正常中止当前会话清理 */
    ecat_transport_endpoint_abort(&ep_uart);
    s_ep_mock_use_override_actual = false;

    /* 12.4 合法空数据回读 (get_data_size == 0) 或已达 EOF 时的立即完成与安全边界 */
    /* 注册 Target 20: 包含 get_data_size 回调返回 0 的空数据区 */
    ecat_target_desc_t empty_desc;
    memset(&empty_desc, 0, sizeof(empty_desc));
    empty_desc.target_id = 20U;
    strncpy(empty_desc.name, "empty_target", sizeof(empty_desc.name));
    empty_desc.type = ECAT_TARGET_TYPE_DATA;
    empty_desc.permissions = ECAT_PERM_READ;
    empty_desc.max_size = 512U;
    empty_desc.enabled = true;
    empty_desc.asynchronous = true;
    empty_desc.driver.async_ops = &ep_async_ops;
    empty_desc.driver.get_data_size = mock_get_size; /* 之后通过静态变量控制返回值 */
    assert(ecat_target_registry_add(&reg, &empty_desc) == 0);

    reset_cb_flags();
    s_mock_size_override = 0U; /* 动态指定空目标数据量为 0 */
    uint32_t empty_total = 0x1234;
    assert(ecat_transport_endpoint_begin_export(&ep_uart, 20U, &empty_total) == ECAT_TRANSFER_COMPLETE);
    assert(empty_total == 0U);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == true);

    size_t eof_actual = 0x9999;
    /* 已到 EOF (remaining == 0) 时，调用 read_chunk 必须立即安全返回 COMPLETE，actual = 0，不误判为驱动错误 */
    assert(ecat_transport_endpoint_read_chunk(
        &ep_uart, zero_prog_buf, sizeof(zero_prog_buf), &eof_actual) == ECAT_TRANSFER_COMPLETE);
    assert(eof_actual == 0U);
    assert(ecat_transfer_get_bytes_transferred(&mgr) == 0U);
    assert(s_cb_aborted_count == 0);
    assert(ecat_transport_endpoint_finish_export(&ep_uart) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_is_active(&ep_uart) == false);
    s_mock_size_override = TEST_FLASH_SIZE; /* 恢复默认 */

    printf(">>> 传输端点接口与自定义通道单元测试全部通过！<<<\n");
    return 0;
}
