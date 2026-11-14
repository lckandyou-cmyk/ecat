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
static int mock_read_open(void *ctx) { (void)ctx; return 0; }
static void mock_read_close(void *ctx) { (void)ctx; }
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
static uint32_t mock_get_size(void *ctx) { (void)ctx; return TEST_FLASH_SIZE; }

/* 回调跟踪 */
static bool s_cb_started = false;
static uint32_t s_cb_progress_last = 0;
static bool s_cb_completed = false;
static bool s_cb_aborted = false;
static uint32_t s_cb_completed_crc = 0;

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
}

static void on_abort(void *ctx, ecat_transfer_err_t err)
{
    (void)ctx; (void)err;
    s_cb_aborted = true;
}

static void reset_cb_flags(void)
{
    s_cb_started = false;
    s_cb_progress_last = 0;
    s_cb_completed = false;
    s_cb_aborted = false;
    s_cb_completed_crc = 0;
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

    ecat_transport_callbacks_t cbs = {
        .on_session_start = on_start,
        .on_progress = on_progress,
        .on_session_complete = on_complete,
        .on_session_abort = on_abort
    };
    ecat_transport_endpoint_set_callbacks(&ep_uart, &cbs);

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

    /* CAN 端点并发争抢拦截 */
    assert(ecat_transport_endpoint_begin_export(&ep_can, 1U, NULL) == ECAT_TRANSFER_BUSY);

    /* 分两包写入 */
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, payload, 200) == ECAT_TRANSFER_COMPLETE);
    assert(s_cb_progress_last == 200);
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, payload + 200, 312) == ECAT_TRANSFER_COMPLETE);
    assert(s_cb_progress_last == 512);

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
    /* 重试写入合法数据 */
    uint8_t retry_buf[16] = {0xAA};
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, retry_buf, 16) == ECAT_TRANSFER_COMPLETE);
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

    printf(">>> 传输端点接口与自定义通道单元测试全部通过！<<<\n");
    return 0;
}
