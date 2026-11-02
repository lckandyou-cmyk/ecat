/**
 * @file test_ecat_foe.c
 * @brief EtherCAT FoE 核心固件升级库全覆盖单元与集成测试
 */

#include "ecat_foe.h"
#include "ecat_foe_ssc.h"
#include "ecat_foe_soes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST_PROJ_ID       0x10010001U
#define TEST_PROJ_NAME     "SERVO_DRV"
#define TEST_HW_ID         0x00000001U
#define MOCK_FLASH_SIZE    (64U * 1024U)

static uint8_t s_mock_flash[MOCK_FLASH_SIZE];
static uint32_t s_erased_size = 0;
static bool s_init_called = false;
static bool s_erase_called = false;
static bool s_finalize_called = false;
static bool s_abort_called = false;

static void reset_mock_state(void)
{
    memset(s_mock_flash, 0xFF, sizeof(s_mock_flash));
    s_erased_size = 0;
    s_init_called = false;
    s_erase_called = false;
    s_finalize_called = false;
    s_abort_called = false;
}

static int mock_app_init(void *ctx)
{
    (void)ctx;
    s_init_called = true;
    return 0;
}

static int mock_app_erase(uint32_t offset, uint32_t size, void *ctx)
{
    (void)ctx;
    s_erase_called = true;
    s_erased_size = size;
    if (offset + size <= sizeof(s_mock_flash)) {
        memset(&s_mock_flash[offset], 0xFF, size);
        return 0;
    }
    return -1;
}

static int mock_app_write(uint32_t offset, const uint8_t *data, uint32_t len, void *ctx)
{
    (void)ctx;
    if (offset + len <= sizeof(s_mock_flash)) {
        memcpy(&s_mock_flash[offset], data, len);
        return 0;
    }
    return -1;
}

static int mock_app_verify(uint32_t total_size, uint32_t expected_crc, void *ctx)
{
    (void)ctx;
    uint32_t crc = ~ecat_crc32_calc(0xFFFFFFFFU, s_mock_flash, total_size);
    return (crc == expected_crc) ? 0 : -1;
}

static int mock_app_finalize(bool success, void *ctx)
{
    (void)ctx;
    s_finalize_called = success;
    return 0;
}

static void mock_app_abort(void *ctx)
{
    (void)ctx;
    s_abort_called = true;
}

static const ecat_foe_target_ops_t s_app_ops = {
    .target_id = ECAT_FOE_TARGET_APP,
    .name = "APP",
    .init = mock_app_init,
    .erase = mock_app_erase,
    .write = mock_app_write,
    .verify = mock_app_verify,
    .finalize = mock_app_finalize,
    .abort = mock_app_abort,
};

static void setup_default_env(void)
{
    reset_mock_state();

    ecat_foe_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.project_id = TEST_PROJ_ID;
    cfg.project_name = TEST_PROJ_NAME;
    cfg.hardware_id = TEST_HW_ID;

    /* APP 目标开启，配额 32KB */
    cfg.partitions[0].target_id = ECAT_FOE_TARGET_APP;
    cfg.partitions[0].enabled = true;
    cfg.partitions[0].base_addr = 0x08020000U;
    cfg.partitions[0].max_size = 32U * 1024U;

    /* FPGA 目标关闭 */
    cfg.partitions[1].target_id = ECAT_FOE_TARGET_FPGA;
    cfg.partitions[1].enabled = false;
    cfg.partitions[1].base_addr = 0x00100000U;
    cfg.partitions[1].max_size = 1024U * 1024U;

    cfg.partition_count = 2;

    int ret = ecat_foe_init(&cfg);
    assert(ret == 0);

    ret = ecat_foe_register_target(&s_app_ops, NULL);
    assert(ret == 0);
}

/* 构造合法的 .efw 完整测试数据流 */
static void make_valid_efw(const uint8_t *payload, uint32_t payload_len,
                           uint32_t proj_id, const char *proj_name,
                           uint32_t hw_id, uint16_t target_id, const char *ver_str,
                           uint8_t *out_buf, size_t *out_total_len)
{
    ecat_efw_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));

    hdr.magic = ECAT_EFW_MAGIC;
    hdr.header_version = ECAT_EFW_HEADER_VERSION;
    hdr.header_size = sizeof(ecat_efw_header_t);
    hdr.project_id = proj_id;
    hdr.hardware_id = hw_id;
    hdr.target_id = target_id;
    hdr.version_raw = 0x01000000U; /* 1.0.0.0 */
    strncpy(hdr.project_name, proj_name, sizeof(hdr.project_name) - 1);
    strncpy(hdr.version_str, ver_str, sizeof(hdr.version_str) - 1);
    hdr.raw_payload_size = payload_len;

    /* 计算 payload CRC32 */
    hdr.raw_payload_crc = ~ecat_crc32_calc(0xFFFFFFFFU, payload, payload_len);

    /* 计算 header CRC32 */
    hdr.header_crc = 0;
    hdr.header_crc = ~ecat_crc32_calc(0xFFFFFFFFU, (const uint8_t *)&hdr, sizeof(hdr));

    memcpy(out_buf, &hdr, sizeof(hdr));
    memcpy(out_buf + sizeof(hdr), payload, payload_len);
    *out_total_len = sizeof(hdr) + payload_len;
}

/* -------------------------------------------------------------------------- */
/*                               各个测试用例                                 */
/* -------------------------------------------------------------------------- */
static void test_normal_upgrade(void)
{
    printf("[TEST] Running test_normal_upgrade...\n");
    setup_default_env();

    /* 准备 1024 字节虚拟固件 payload */
    uint8_t payload[1024];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(i & 0xFF);
    }

    uint8_t efw_stream[2048];
    size_t total_efw_len = 0;
    make_valid_efw(payload, sizeof(payload), TEST_PROJ_ID, TEST_PROJ_NAME,
                   TEST_HW_ID, ECAT_FOE_TARGET_APP, "1.0.0",
                   efw_stream, &total_efw_len);

    uint16_t err = 0;
    const char *fn = "SERVO_DRV_APP_1.0.0.efw";
    bool ok = ecat_foe_on_wrq(fn, (uint32_t)strlen(fn), 0, &err);
    assert(ok && err == ECAT_FOE_ERR_OK);
    assert(ecat_foe_get_state() == ECAT_FOE_STATE_WRQ_ACKED);

    /* 分包发送：每包 256 字节 (典型 Bootstrap 邮箱尺寸) */
    size_t chunk_size = 256;
    size_t sent = 0;
    uint32_t pkt_no = 1;

    while (sent < total_efw_len) {
        size_t remaining = total_efw_len - sent;
        size_t this_chunk = (remaining < chunk_size) ? remaining : chunk_size;
        bool is_last = (sent + this_chunk == total_efw_len);

        ok = ecat_foe_on_data(pkt_no, &efw_stream[sent], (uint32_t)this_chunk, is_last, &err);
        assert(ok && err == ECAT_FOE_ERR_OK);

        sent += this_chunk;
        pkt_no++;
    }

    assert(ecat_foe_get_state() == ECAT_FOE_STATE_COMPLETED);
    assert(s_init_called);
    assert(s_erase_called);
    assert(s_erased_size == sizeof(payload));
    assert(s_finalize_called);
    assert(!s_abort_called);

    /* 验证 mock flash 中烧写的内容与 payload 严格一致 */
    assert(memcmp(s_mock_flash, payload, sizeof(payload)) == 0);
    printf("  -> PASS\n");
}

static void test_filename_project_mismatch(void)
{
    printf("[TEST] Running test_filename_project_mismatch...\n");
    setup_default_env();

    uint16_t err = 0;
    const char *fn = "WRONGPROJ_APP_1.0.0.efw";
    bool ok = ecat_foe_on_wrq(fn, (uint32_t)strlen(fn), 0, &err);
    assert(!ok);
    assert(err == ECAT_FOE_ERR_ACCESS_DENIED);
    assert(ecat_foe_get_state() == ECAT_FOE_STATE_ERROR);
    printf("  -> PASS\n");
}

static void test_target_disabled_in_config(void)
{
    printf("[TEST] Running test_target_disabled_in_config...\n");
    setup_default_env();

    uint16_t err = 0;
    /* FPGA 在配置中明确 disabled */
    const char *fn = "SERVO_DRV_FPGA_1.0.0.efw";
    bool ok = ecat_foe_on_wrq(fn, (uint32_t)strlen(fn), 0, &err);
    assert(!ok);
    assert(err == ECAT_FOE_ERR_ACCESS_DENIED);
    printf("  -> PASS\n");
}

static void test_header_project_id_mismatch(void)
{
    printf("[TEST] Running test_header_project_id_mismatch...\n");
    setup_default_env();

    uint8_t payload[64] = {0xAA};
    uint8_t efw_stream[512];
    size_t total_efw_len = 0;
    /* 构造一个 Project ID 错误的头部 (0x99999999) */
    make_valid_efw(payload, sizeof(payload), 0x99999999U, TEST_PROJ_NAME,
                   TEST_HW_ID, ECAT_FOE_TARGET_APP, "1.0.0",
                   efw_stream, &total_efw_len);

    uint16_t err = 0;
    const char *fn = "SERVO_DRV_APP_1.0.0.efw";
    bool ok = ecat_foe_on_wrq(fn, (uint32_t)strlen(fn), 0, &err);
    assert(ok);

    /* 发送首包，包含篡改过的头 */
    ok = ecat_foe_on_data(1, efw_stream, 128, false, &err);
    assert(!ok);
    assert(err == ECAT_FOE_ERR_ACCESS_DENIED);
    assert(ecat_foe_get_state() == ECAT_FOE_STATE_ERROR);
    assert(s_abort_called);
    printf("  -> PASS\n");
}

static void test_header_target_id_mismatch(void)
{
    printf("[TEST] Running test_header_target_id_mismatch...\n");
    setup_default_env();

    uint8_t payload[64] = {0x55};
    uint8_t efw_stream[512];
    size_t total_efw_len = 0;
    /* 文件名请求的是 APP，但文件头内 target_id 标的是 EEPROM */
    make_valid_efw(payload, sizeof(payload), TEST_PROJ_ID, TEST_PROJ_NAME,
                   TEST_HW_ID, ECAT_FOE_TARGET_EEPROM, "1.0.0",
                   efw_stream, &total_efw_len);

    uint16_t err = 0;
    const char *fn = "SERVO_DRV_APP_1.0.0.efw";
    bool ok = ecat_foe_on_wrq(fn, (uint32_t)strlen(fn), 0, &err);
    assert(ok);

    ok = ecat_foe_on_data(1, efw_stream, 128, false, &err);
    assert(!ok);
    assert(err == ECAT_FOE_ERR_ILLEGAL);
    assert(ecat_foe_get_state() == ECAT_FOE_STATE_ERROR);
    printf("  -> PASS\n");
}

static void test_payload_overflow_partition(void)
{
    printf("[TEST] Running test_payload_overflow_partition...\n");
    setup_default_env();

    /* 分区上限配置为 32KB，这里宣称 payload 大小为 64KB */
    uint8_t payload[64] = {0};
    uint8_t efw_stream[512];
    size_t total_efw_len = 0;
    make_valid_efw(payload, sizeof(payload), TEST_PROJ_ID, TEST_PROJ_NAME,
                   TEST_HW_ID, ECAT_FOE_TARGET_APP, "1.0.0",
                   efw_stream, &total_efw_len);

    /* 手动篡改头部宣称大小为 48KB 并重算 header CRC */
    ecat_efw_header_t *hdr = (ecat_efw_header_t *)efw_stream;
    hdr->raw_payload_size = 48U * 1024U;
    hdr->header_crc = 0;
    hdr->header_crc = ~ecat_crc32_calc(0xFFFFFFFFU, (const uint8_t *)hdr, sizeof(ecat_efw_header_t));

    uint16_t err = 0;
    const char *fn = "SERVO_DRV_APP_1.0.0.efw";
    bool ok = ecat_foe_on_wrq(fn, (uint32_t)strlen(fn), 0, &err);
    assert(ok);

    ok = ecat_foe_on_data(1, efw_stream, 128, false, &err);
    assert(!ok);
    assert(err == ECAT_FOE_ERR_DISK_FULL);
    assert(ecat_foe_get_state() == ECAT_FOE_STATE_ERROR);
    printf("  -> PASS\n");
}

static void test_payload_crc_corruption(void)
{
    printf("[TEST] Running test_payload_crc_corruption...\n");
    setup_default_env();

    uint8_t payload[100] = {1, 2, 3, 4};
    uint8_t efw_stream[512];
    size_t total_efw_len = 0;
    make_valid_efw(payload, sizeof(payload), TEST_PROJ_ID, TEST_PROJ_NAME,
                   TEST_HW_ID, ECAT_FOE_TARGET_APP, "1.0.0",
                   efw_stream, &total_efw_len);

    /* 故意篡改 payload 中最后 1 个字节 */
    efw_stream[total_efw_len - 1] ^= 0xFF;

    uint16_t err = 0;
    const char *fn = "SERVO_DRV_APP_1.0.0.efw";
    assert(ecat_foe_on_wrq(fn, (uint32_t)strlen(fn), 0, &err));

    /* 发送整个包 (is_last = true) */
    bool ok = ecat_foe_on_data(1, efw_stream, (uint32_t)total_efw_len, true, &err);
    assert(!ok);
    assert(err == ECAT_FOE_ERR_PROG_ERROR);
    assert(ecat_foe_get_state() == ECAT_FOE_STATE_ERROR);
    assert(s_abort_called);
    printf("  -> PASS\n");
}

static void test_ssc_and_soes_adapters(void)
{
    printf("[TEST] Running test_ssc_and_soes_adapters...\n");
    setup_default_env();

    uint8_t payload[64] = {0x42};
    uint8_t efw_stream[512];
    size_t total_efw_len = 0;
    make_valid_efw(payload, sizeof(payload), TEST_PROJ_ID, TEST_PROJ_NAME,
                   TEST_HW_ID, ECAT_FOE_TARGET_APP, "1.0.0",
                   efw_stream, &total_efw_len);

    /* 1. 测试 SSC 转接层 */
    const char *fn = "SERVO_DRV_APP_1.0.0.efw";
    uint16_t ret_ssc = ecat_foe_ssc_write(fn, (uint16_t)strlen(fn), 0);
    assert(ret_ssc == ECAT_FOE_ERR_OK);

    ret_ssc = ecat_foe_ssc_data(efw_stream, (uint16_t)total_efw_len, true);
    assert(ret_ssc == ECAT_FOE_ERR_OK);
    assert(ecat_foe_get_state() == ECAT_FOE_STATE_COMPLETED);

    /* 重置并测试 SOES 转接层 */
    setup_default_env();
    uint16_t ret_soes = ecat_foe_soes_write(NULL, (uint8_t *)fn, (uint32_t)strlen(fn), 0);
    assert(ret_soes == ECAT_FOE_ERR_OK);

    ret_soes = ecat_foe_soes_write_data(NULL, efw_stream, total_efw_len, 1);
    assert(ret_soes == ECAT_FOE_ERR_OK);
    assert(ecat_foe_get_state() == ECAT_FOE_STATE_COMPLETED);

    printf("  -> PASS\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("   Running ecat FoE Core Unit Tests\n");
    printf("=========================================\n");

    test_normal_upgrade();
    test_filename_project_mismatch();
    test_target_disabled_in_config();
    test_header_project_id_mismatch();
    test_header_target_id_mismatch();
    test_payload_overflow_partition();
    test_payload_crc_corruption();
    test_ssc_and_soes_adapters();

    printf("=========================================\n");
    printf("   ALL 8 TEST SUITES PASSED (100%%)\n");
    printf("=========================================\n");
    return 0;
}
