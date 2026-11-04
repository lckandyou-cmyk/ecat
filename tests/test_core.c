/**
 * @file test_core.c
 * @brief EtherCAT FoE 核心状态机与 SOES 适配层全量功能测试套件 (阶段 B)
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "ecat_foe_core.h"
#include "ecat_foe_soes.h"
#include "mock_driver.h"

static int g_core_tests_run = 0;
static int g_core_tests_passed = 0;
static int g_core_tests_failed = 0;
static bool g_current_test_failed = false;

#define TEST_ASSERT(cond, msg) do { \
    g_core_tests_run++; \
    if (!(cond)) { \
        printf("\n    [ASSERT FAIL] %s:%d: %s", __func__, __LINE__, msg); \
        g_core_tests_failed++; \
        g_current_test_failed = true; \
        return; \
    } else { \
        g_core_tests_passed++; \
    } \
} while(0)

#define TEST_ASSERT_EQ_INT(actual, expected, msg) do { \
    g_core_tests_run++; \
    if ((actual) != (expected)) { \
        printf("\n    [ASSERT FAIL] %s:%d: %s (实际: %d, 期望: %d)", \
               __func__, __LINE__, msg, (int)(actual), (int)(expected)); \
        g_core_tests_failed++; \
        g_current_test_failed = true; \
        return; \
    } else { \
        g_core_tests_passed++; \
    } \
} while(0)

#define RUN_TEST(fn) do { \
    g_current_test_failed = false; \
    printf("  RUN   %-55s", #fn); \
    fn(); \
    if (!g_current_test_failed) { \
        printf(" [PASS]\n"); \
    } else { \
        printf(" [FAIL]\n"); \
    } \
} while(0)

/* -------------------------------------------------------------------------- */
/*                            平台防回滚桩接口实现                            */
/* -------------------------------------------------------------------------- */
static uint32_t s_mock_version_floors[ECAT_FOE_TARGET_COUNT] = {0};
static bool s_mock_get_floor_fail = false;
static bool s_mock_set_floor_fail = false;
static uint16_t s_mock_floor_fail_target_id = 0;

int port_get_version_floor(uint16_t target_id, uint32_t *floor_ver)
{
    if (s_mock_get_floor_fail) {
        return -1;
    }
    if (s_mock_floor_fail_target_id != 0 && target_id == s_mock_floor_fail_target_id) {
        return -1;
    }
    if (target_id >= 1 && target_id <= ECAT_FOE_TARGET_COUNT) {
        if (floor_ver != NULL) {
            *floor_ver = s_mock_version_floors[target_id - 1];
        }
        return 0;
    }
    return -1;
}

int port_set_version_floor(uint16_t target_id, uint32_t new_floor_ver)
{
    if (s_mock_set_floor_fail) {
        return -1;
    }
    if (target_id >= 1 && target_id <= ECAT_FOE_TARGET_COUNT) {
        s_mock_version_floors[target_id - 1] = new_floor_ver;
        return 0;
    }
    return -1;
}

/* -------------------------------------------------------------------------- */
/*                           辅助构造与流式推送函数                           */
/* -------------------------------------------------------------------------- */

static size_t build_test_efw(
    uint8_t *out_buf,
    uint32_t project_id,
    uint32_t hardware_id,
    uint16_t hw_ver_min,
    uint16_t hw_ver_max,
    uint16_t target_id,
    uint16_t flags,
    uint32_t fw_version,
    uint32_t min_allow_version,
    const uint8_t *payload,
    uint32_t payload_len)
{
    memset(out_buf, 0, 64);
    /* 0x00: magic */
    out_buf[0] = 0x45; out_buf[1] = 0x43; out_buf[2] = 0x46; out_buf[3] = 0x57;
    /* 0x04: format_version = 1 */
    out_buf[4] = 0x01; out_buf[5] = 0x00;
    /* 0x06: header_length = 64 */
    out_buf[6] = 0x40; out_buf[7] = 0x00;
    /* 0x08: project_id */
    out_buf[8] = (uint8_t)(project_id & 0xFF);
    out_buf[9] = (uint8_t)((project_id >> 8) & 0xFF);
    out_buf[10] = (uint8_t)((project_id >> 16) & 0xFF);
    out_buf[11] = (uint8_t)((project_id >> 24) & 0xFF);
    /* 0x0C: hardware_id */
    out_buf[12] = (uint8_t)(hardware_id & 0xFF);
    out_buf[13] = (uint8_t)((hardware_id >> 8) & 0xFF);
    out_buf[14] = (uint8_t)((hardware_id >> 16) & 0xFF);
    out_buf[15] = (uint8_t)((hardware_id >> 24) & 0xFF);
    /* 0x10: hw_ver_min */
    out_buf[16] = (uint8_t)(hw_ver_min & 0xFF);
    out_buf[17] = (uint8_t)((hw_ver_min >> 8) & 0xFF);
    /* 0x12: hw_ver_max */
    out_buf[18] = (uint8_t)(hw_ver_max & 0xFF);
    out_buf[19] = (uint8_t)((hw_ver_max >> 8) & 0xFF);
    /* 0x14: target_id */
    out_buf[20] = (uint8_t)(target_id & 0xFF);
    out_buf[21] = (uint8_t)((target_id >> 8) & 0xFF);
    /* 0x16: flags */
    out_buf[22] = (uint8_t)(flags & 0xFF);
    out_buf[23] = (uint8_t)((flags >> 8) & 0xFF);
    /* 0x18: fw_version */
    out_buf[24] = (uint8_t)(fw_version & 0xFF);
    out_buf[25] = (uint8_t)((fw_version >> 8) & 0xFF);
    out_buf[26] = (uint8_t)((fw_version >> 16) & 0xFF);
    out_buf[27] = (uint8_t)((fw_version >> 24) & 0xFF);
    /* 0x1C: min_allow_version */
    out_buf[28] = (uint8_t)(min_allow_version & 0xFF);
    out_buf[29] = (uint8_t)((min_allow_version >> 8) & 0xFF);
    out_buf[30] = (uint8_t)((min_allow_version >> 16) & 0xFF);
    out_buf[31] = (uint8_t)((min_allow_version >> 24) & 0xFF);
    /* 0x20: payload_length */
    out_buf[32] = (uint8_t)(payload_len & 0xFF);
    out_buf[33] = (uint8_t)((payload_len >> 8) & 0xFF);
    out_buf[34] = (uint8_t)((payload_len >> 16) & 0xFF);
    out_buf[35] = (uint8_t)((payload_len >> 24) & 0xFF);

    /* 0x24: payload_crc32 */
    uint32_t p_crc = (payload != NULL && payload_len > 0) ? ecat_foe_crc32_calc(payload, payload_len) : 0U;
    out_buf[36] = (uint8_t)(p_crc & 0xFF);
    out_buf[37] = (uint8_t)((p_crc >> 8) & 0xFF);
    out_buf[38] = (uint8_t)((p_crc >> 16) & 0xFF);
    out_buf[39] = (uint8_t)((p_crc >> 24) & 0xFF);

    /* 0x28 - 0x3B: reserved (20 bytes all 0x00) */

    /* 0x3C: header_crc32 */
    uint32_t h_crc = ecat_foe_crc32_calc(out_buf, 60);
    out_buf[60] = (uint8_t)(h_crc & 0xFF);
    out_buf[61] = (uint8_t)((h_crc >> 8) & 0xFF);
    out_buf[62] = (uint8_t)((h_crc >> 16) & 0xFF);
    out_buf[63] = (uint8_t)((h_crc >> 24) & 0xFF);

    if (payload != NULL && payload_len > 0) {
        memcpy(out_buf + 64, payload, payload_len);
    }
    return 64U + payload_len;
}

static int test_wrq(ecat_foe_core_t *core, const char *fn)
{
    return ecat_foe_core_on_wrq(core, fn, (fn != NULL) ? strlen(fn) : 0);
}

static void setup_default_core(ecat_foe_core_t *core, mock_driver_t *mock_mcu, mock_driver_t *mock_sii, mock_driver_t *mock_eeprom)
{
    ecat_foe_core_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.project_id = 0x10010001U;
    cfg.project_name = "SERVO_DRV";
    cfg.hardware_id = 0x00000001U;
    cfg.local_hw_ver = 1U;
    cfg.anti_rollback_enable = false;
    cfg.max_header_size = 128U;

    /* Target 1: MCU */
    cfg.targets[0].target_id = 1;
    cfg.targets[0].enabled = (mock_mcu != NULL);
    cfg.targets[0].max_payload_size = 384U * 1024U;
    cfg.targets[0].write_unit = 8U;
    cfg.targets[0].driver = (mock_mcu != NULL) ? &mock_mcu->driver : NULL;
    cfg.targets[0].user_ctx = mock_mcu;

    /* Target 2: FPGA (禁用) */
    cfg.targets[1].target_id = 2;
    cfg.targets[1].enabled = false;

    /* Target 3: SII */
    cfg.targets[2].target_id = 3;
    cfg.targets[2].enabled = (mock_sii != NULL);
    cfg.targets[2].max_payload_size = 4U * 1024U;
    cfg.targets[2].write_unit = 2U;
    cfg.targets[2].driver = (mock_sii != NULL) ? &mock_sii->driver : NULL;
    cfg.targets[2].user_ctx = mock_sii;

    /* Target 4: EEPROM */
    cfg.targets[3].target_id = 4;
    cfg.targets[3].enabled = (mock_eeprom != NULL);
    cfg.targets[3].max_payload_size = 8U * 1024U;
    cfg.targets[3].write_unit = 4U;
    cfg.targets[3].driver = (mock_eeprom != NULL) ? &mock_eeprom->driver : NULL;
    cfg.targets[3].user_ctx = mock_eeprom;

    s_mock_get_floor_fail = false;
    s_mock_set_floor_fail = false;

    int ret_init = ecat_foe_core_init(core, &cfg);
    (void)ret_init;
}

static int feed_stream_chunks(
    ecat_foe_core_t *core,
    const uint8_t *stream,
    size_t total_len,
    size_t chunk_size,
    bool send_zero_len)
{
    size_t off = 0;
    while (off < total_len) {
        size_t cur = total_len - off;
        if (cur > chunk_size) {
            cur = chunk_size;
        }
        int ret = ecat_foe_core_on_data(core, stream + off, cur);
        if (ret != 0) {
            return ret;
        }
        off += cur;
    }
    if (send_zero_len) {
        return ecat_foe_core_on_data(core, NULL, 0);
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/*                     测试场景详细实现 (与要求一一对应)                       */
/* -------------------------------------------------------------------------- */

/* 1. 正常升级 MCU */
static void test_normal_upgrade_mcu(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    uint8_t payload[32];
    for (int i = 0; i < 32; i++) payload[i] = (uint8_t)(i + 1);

    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01020304U, 0, payload, 32);

    int ret_wrq = test_wrq(&core, "SERVO_DRV_MCU_V1.2.3.4.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "MCU WRQ 应当成功");
    int ret_data = feed_stream_chunks(&core, efw, efw_len, 64, false);
    TEST_ASSERT_EQ_INT(ret_data, 0, "MCU 数据块推送应当成功");
    int ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, 0, "MCU 提交应当成功");
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_SUCCESS, "状态应为 SUCCESS");
    TEST_ASSERT(mcu.valid_flag, "MCU 固件标记应有效");
}

/* 2. 正常升级 SII (偶数长度) */
static void test_normal_upgrade_sii(void)
{
    mock_driver_t sii;
    mock_driver_init(&sii);
    ecat_foe_core_t core;
    setup_default_core(&core, NULL, &sii, NULL);

    uint8_t payload[16];
    for (int i = 0; i < 16; i++) payload[i] = (uint8_t)(i * 2);

    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 3, 0, 0x01000000U, 0, payload, 16);

    int ret_wrq = test_wrq(&core, "SERVO_DRV_SII_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "SII WRQ 应当成功");
    int ret_data = feed_stream_chunks(&core, efw, efw_len, 32, false);
    TEST_ASSERT_EQ_INT(ret_data, 0, "SII 数据块推送应当成功");
    int ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, 0, "SII 提交应当成功");
    TEST_ASSERT(sii.valid_flag, "SII 固件标记应有效");
}

/* 3. 正常升级 EEPROM */
static void test_normal_upgrade_eeprom(void)
{
    mock_driver_t eeprom;
    mock_driver_init(&eeprom);
    ecat_foe_core_t core;
    setup_default_core(&core, NULL, NULL, &eeprom);

    uint8_t payload[24];
    for (int i = 0; i < 24; i++) payload[i] = (uint8_t)(i + 5);

    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 4, 0, 0x01000100U, 0, payload, 24);

    int ret_wrq = test_wrq(&core, "SERVO_DRV_EEPROM_V1.0.1.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "EEPROM WRQ 应当成功");
    int ret_data = feed_stream_chunks(&core, efw, efw_len, 20, false);
    TEST_ASSERT_EQ_INT(ret_data, 0, "EEPROM 数据块推送应当成功");
    int ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, 0, "EEPROM 提交应当成功");
    TEST_ASSERT(eeprom.valid_flag, "EEPROM 固件标记应有效");
}

/* 4. buffer_size 16 头跨块 */
static void test_buffer_size_16_header_cross_chunks(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    uint8_t payload[16] = {1, 2, 3};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    int ret = feed_stream_chunks(&core, efw, efw_len, 16, false);
    TEST_ASSERT_EQ_INT(ret, 0, "16 字节切块跨块推送应当成功");
    int ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, 0, "提交应当成功");
}

/* 5. buffer_size 32 头跨块 */
static void test_buffer_size_32_header_cross_chunks(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    uint8_t payload[32] = {4, 5, 6};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 32);

    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    int ret = feed_stream_chunks(&core, efw, efw_len, 32, false);
    TEST_ASSERT_EQ_INT(ret, 0, "32 字节切块跨块推送应当成功");
    int ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, 0, "提交应当成功");
}

/* 6. 0 长度结束包与整除情形 */
static void test_zero_length_chunk_and_exact_division(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    /* header 64 + payload 64 = 128 字节，按 64 字节切块恰好整除 (2 块) */
    uint8_t payload[64];
    memset(payload, 0x33, sizeof(payload));
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 64);

    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    /* 发送两个 64 字节块，随后紧跟 0 长度包结束 */
    int ret = feed_stream_chunks(&core, efw, efw_len, 64, true);
    TEST_ASSERT_EQ_INT(ret, 0, "整除加 0 长度包推送应当成功");
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_SUCCESS, "状态成功");
}

/* 7. 头阶段各拒绝场景驱动调用为 0 */
static void test_header_rejections_driver_zero_calls(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;

    uint8_t efw[128];

    /* A. magic 错 */
    setup_default_core(&core, &mcu, NULL, NULL);
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, NULL, 0);
    efw[0] ^= 0xFF; /* 破坏 magic */
    int ret = ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT(ret != 0, "magic 错被拒");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "magic 错驱动 init 必须为 0");
    TEST_ASSERT_EQ_INT(mcu.count_invalidate, 0, "magic 错驱动 invalidate 必须为 0");

    /* B. format_version 不是 1 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, NULL, 0);
    efw[4] = 2; /* version = 2 */
    ret = ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT(ret != 0, "version 错被拒");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "version 错驱动 init 必须为 0");

    /* C. flags 非零 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0x0001U, 0x01000000U, 0, NULL, 0);
    ret = ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT(ret != 0, "flags 非零被拒");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "flags 非零驱动 init 必须为 0");

    /* D. Header CRC 错 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, NULL, 0);
    efw[63] ^= 0xFF;
    ret = ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT(ret != 0, "Header CRC 错被拒");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "Header CRC 错驱动 init 必须为 0");
}

/* 8. 文件名拒绝驱动调用为 0 (授权改动 3.a 验证: 格式错回 0x8004，项目不符/目标禁用回 0x8001) */
static void test_filename_rejection_driver_zero_calls(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;

    /* A. 文件名格式不对 -> 0x8004 (FOE_ERR_ILLEGAL) */
    setup_default_core(&core, &mcu, NULL, NULL);
    int ret = test_wrq(&core, "INVALID_FORMAT.bin");
    TEST_ASSERT_EQ_INT(ret, ECAT_FOE_ERRCODE_ILLEGAL, "格式错应回 0x8004");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "驱动调用必须为 0");

    /* B. 项目名不符 -> 0x8001 (FOE_ERR_NOTFOUND) */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    ret = test_wrq(&core, "WRONG_PROJ_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret, ECAT_FOE_ERRCODE_NOTFOUND, "项目名不符应回 0x8001");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "驱动调用必须为 0");

    /* C. 目标字样不存在 -> 0x8001 (FOE_ERR_NOTFOUND) */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    ret = test_wrq(&core, "SERVO_DRV_UNKNOWN_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret, ECAT_FOE_ERRCODE_NOTFOUND, "目标不存在应回 0x8001");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "驱动调用必须为 0");

    /* D. 目标未启用 (FPGA) -> 0x8001 (FOE_ERR_NOTFOUND) */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    ret = test_wrq(&core, "SERVO_DRV_FPGA_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret, ECAT_FOE_ERRCODE_NOTFOUND, "目标禁用应回 0x8001");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "驱动调用必须为 0");
}

/* 9. 调用顺序断言与 finalize / abort 互斥 */
static void test_call_sequence_and_mutex(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    uint8_t payload[16] = {1, 2, 3};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    feed_stream_chunks(&core, efw, efw_len, 64, false);
    ecat_foe_core_on_end(&core);

    /* 验证顺序: init -> invalidate -> erase_to -> write -> read -> finalize */
    TEST_ASSERT(mcu.op_count >= 6, "操作数应当 >= 6");
    TEST_ASSERT_EQ_INT(mcu.op_history[0], MOCK_OP_INIT, "第 1 步 init");
    TEST_ASSERT_EQ_INT(mcu.op_history[1], MOCK_OP_INVALIDATE, "第 2 步 invalidate (严格早于 erase)");
    TEST_ASSERT_EQ_INT(mcu.op_history[2], MOCK_OP_ERASE_TO, "第 3 步 erase_to");
    TEST_ASSERT_EQ_INT(mcu.op_history[3], MOCK_OP_WRITE, "第 4 步 write");
    TEST_ASSERT_EQ_INT(mcu.op_history[mcu.op_count - 2], MOCK_OP_READ, "倒数第 2 步 read");
    TEST_ASSERT_EQ_INT(mcu.op_history[mcu.op_count - 1], MOCK_OP_FINALIZE, "最后 1 步 finalize");

    /* 互斥断言: 成功时 finalize 为 1，abort 必须为 0 */
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 1, "finalize 应被调 1 次");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "abort 绝对不可被调 (互斥)");
}

/* 10. 少收与多收整块拒绝不写 */
static void test_stream_length_under_and_over_receive(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;

    uint8_t payload[16] = {1, 2, 3};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    (void)efw_len;

    /* A. 少收 */
    setup_default_core(&core, &mcu, NULL, NULL);
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    ecat_foe_core_on_data(&core, efw, 64); /* 仅发头 */
    int ret = ecat_foe_core_on_end(&core);
    TEST_ASSERT(ret != 0, "少收应当报错");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_UNDER_RECEIVE, "应当报 UNDER_RECEIVE");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "少收必须调用 abort");
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 0, "少收 finalize 为 0");

    /* B. 多收整块拒绝不写 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    ecat_foe_core_on_data(&core, efw, 64 + 8);
    uint32_t writes_before = mcu.count_write;

    uint8_t over_chunk[20];
    memset(over_chunk, 0xAA, sizeof(over_chunk));
    int ret_over = ecat_foe_core_on_data(&core, over_chunk, sizeof(over_chunk));
    TEST_ASSERT(ret_over != 0, "多收整块必须拒绝");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_OVER_RECEIVE, "应当报 OVER_RECEIVE");
    TEST_ASSERT_EQ_INT(mcu.count_write, writes_before, "多收块绝对不可写入底层");
}

/* 11. 流 CRC 错与 readback 损坏 */
static void test_crc_and_readback_errors(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;

    uint8_t payload[16] = {1, 2, 3};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    /* A. 流 CRC 错 */
    setup_default_core(&core, &mcu, NULL, NULL);
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    uint8_t bad_stream[256];
    memcpy(bad_stream, efw, efw_len);
    bad_stream[64] ^= 0xFF; /* 篡改负载 */
    feed_stream_chunks(&core, bad_stream, efw_len, 64, false);
    int ret = ecat_foe_core_on_end(&core);
    TEST_ASSERT(ret != 0, "流 CRC 错拒绝提交");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_STREAM_CRC_MISMATCH, "报 STREAM_CRC_MISMATCH");

    /* B. readback 损坏 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    mcu.inject_corrupt_read = true;
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    feed_stream_chunks(&core, efw, efw_len, 64, false);
    ret = ecat_foe_core_on_end(&core);
    TEST_ASSERT(ret != 0, "回读损坏拒绝提交");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_READBACK_CRC_MISMATCH, "报 READBACK_CRC_MISMATCH");
}

/* 12. 驱动操作失败 (erase/write/finalize) */
static void test_driver_operation_failures(void)
{
    mock_driver_t mcu;
    ecat_foe_core_t core;
    uint8_t payload[16] = {1, 2, 3};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    /* A. erase 失败 */
    mock_driver_init(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    mcu.inject_fail_erase = true;
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    int ret = feed_stream_chunks(&core, efw, efw_len, 64, false);
    TEST_ASSERT(ret != 0, "erase 失败");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_DRV_ERASE, "报 DRV_ERASE");

    /* B. write 失败 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    mcu.inject_fail_write = true;
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    ret = feed_stream_chunks(&core, efw, efw_len, 64, false);
    TEST_ASSERT(ret != 0, "write 失败");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_DRV_WRITE, "报 DRV_WRITE");

    /* C. finalize 失败 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    mcu.inject_fail_finalize = true;
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    feed_stream_chunks(&core, efw, efw_len, 64, false);
    ret = ecat_foe_core_on_end(&core);
    TEST_ASSERT(ret != 0, "finalize 失败");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_DRV_FINALIZE, "报 DRV_FINALIZE");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "finalize 失败转调 abort");
}

/* 13. on_abort 与栈中止后直接新 on_wrq (自愈) */
static void test_on_abort_and_stack_abort_self_healing(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    uint8_t payload[16] = {1, 2};
    uint8_t efw[256];
    build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    /* A. 中途调用 on_abort */
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_DATA, "已进入 DATA 态");
    ecat_foe_core_on_abort(&core);
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_IDLE, "on_abort 后重置为 IDLE");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "调用驱动 abort");

    /* B. 模拟协议栈中止未调 abort，直接下发新 on_wrq 自愈 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    ecat_foe_core_on_data(&core, efw, 64);
    int ret_heal = test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_heal, 0, "新会话自愈成功");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "上一残留驱动 abort 被成功自愈触发");
}

/* 14. 容量边界 (恰好等于容量 vs 超 1 字节) */
static void test_capacity_boundary_exact_and_plus_one(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    core.cfg.targets[0].max_payload_size = 64U;
    uint8_t payload[65];
    memset(payload, 0x77, sizeof(payload));
    uint8_t efw[256];

    /* 恰好等于 64 字节 -> 成功 */
    build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 64);
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    int ret = ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT_EQ_INT(ret, 0, "容量等于上限应当通过");

    /* 超 1 字节 -> 拒绝 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    core.cfg.targets[0].max_payload_size = 64U;
    build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 65);
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    ret = ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT(ret != 0, "容量超 1 字节必须拒绝");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "超容量驱动 init 绝不能调");
}

/* 15. write_unit 8 和 256 错位残留写入、回读数据一致性与 readback CRC 不含 0xFF 填充 */
static void test_write_unit_8_and_256_residual_and_readback_crc_excludes_padding(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;

    /* A. write_unit = 8, payload = 11 字节 (末尾补 5 个 0xFF 写入物理介质) */
    setup_default_core(&core, &mcu, NULL, NULL);
    core.cfg.targets[0].write_unit = 8U;
    uint8_t payload11[11] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload11, 11);

    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    feed_stream_chunks(&core, efw, efw_len, 32, false);
    int ret = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret, 0, "残留写入与回读比对成功");
    /* 物理介质中前 11 字节与 payload 完全一致，后 5 字节为 0xFF */
    TEST_ASSERT(memcmp(mcu.mem, payload11, 11) == 0, "前 11 字节一致");
    for (int i = 11; i < 16; i++) {
        TEST_ASSERT_EQ_INT(mcu.mem[i], 0xFF, "填充字节为 0xFF");
    }

    /* B. write_unit = 256, payload = 270 字节 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    core.cfg.targets[0].write_unit = 256U;
    uint8_t payload270[270];
    for (int i = 0; i < 270; i++) payload270[i] = (uint8_t)(i + 0x30);
    uint8_t efw_large[512];
    efw_len = build_test_efw(efw_large, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload270, 270);

    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    feed_stream_chunks(&core, efw_large, efw_len, 64, false);
    ret = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret, 0, "write_unit 256 升级成功");
    TEST_ASSERT(memcmp(mcu.mem, payload270, 270) == 0, "回读数据完全吻合 (不含填充字节)");
}

/* 16. 防回滚行为全量覆盖 (授权改动 3.b 验证) */
static void test_anti_rollback_full_behavior(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    uint8_t payload[16] = {0};
    uint8_t efw[256];

    /* A. 防回滚开启时 port_get_version_floor 返回非零 -> 拒绝升级 */
    setup_default_core(&core, &mcu, NULL, NULL);
    core.cfg.anti_rollback_enable = true;
    s_mock_get_floor_fail = true; /* 注入获取失败 */
    size_t len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x02000000U, 0, payload, 16);
    test_wrq(&core, "SERVO_DRV_MCU_V2.0.0.0.efw");
    int ret = ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT(ret != 0, "get floor 失败必须拒绝升级");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_ANTI_ROLLBACK_GET, "报 ANTI_ROLLBACK_GET");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "get floor 失败驱动不能调 init");
    s_mock_get_floor_fail = false;

    /* B. 防回滚降级拦截 (V1.0 拦截尝试刷入 V0.9) */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    core.cfg.anti_rollback_enable = true;
    port_set_version_floor(1, 0x01000000U);
    len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x00090000U, 0, payload, 16);
    test_wrq(&core, "SERVO_DRV_MCU_V0.9.0.0.efw");
    ret = ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT(ret != 0, "降级必须被拦截");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_HDR_VERIFY, "报 HDR_VERIFY");

    /* C. 正常升级成功后持久化更新下限 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    core.cfg.anti_rollback_enable = true;
    port_set_version_floor(1, 0x01000000U);
    len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x02000000U, 0x01080000U, payload, 16);
    test_wrq(&core, "SERVO_DRV_MCU_V2.0.0.0.efw");
    feed_stream_chunks(&core, efw, len, 64, false);
    ret = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret, 0, "升级成功");
    uint32_t floor_ver = 0;
    port_get_version_floor(1, &floor_ver);
    TEST_ASSERT_EQ_INT(floor_ver, 0x01080000U, "版本下限成功更新为 0x01080000");

    /* D. finalize 成功后 port_set_version_floor 失败 -> 返回错误，固件保持有效，不调 abort */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    core.cfg.anti_rollback_enable = true;
    port_set_version_floor(1, 0x01000000U);
    s_mock_set_floor_fail = true; /* 注入更新失败 */

    len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x02000000U, 0x01080000U, payload, 16);
    test_wrq(&core, "SERVO_DRV_MCU_V2.0.0.0.efw");
    feed_stream_chunks(&core, efw, len, 64, false);
    ret = ecat_foe_core_on_end(&core);
    TEST_ASSERT(ret != 0, "更新下限失败必须向主站返回错误");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_ANTI_ROLLBACK_UPDATE, "报 ANTI_ROLLBACK_UPDATE");
    TEST_ASSERT(mcu.valid_flag, "固件已 finalize 提交，必须保持物理有效状态");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "固件保持有效，绝对不调 abort");
    s_mock_set_floor_fail = false;
}

/* 17. SOES 适配层短包与 0 长度包及 post hook 状态拦截 */
static void test_soes_adapter_short_packet_and_post_hook(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    ecat_foe_soes_adapter_t adapter;
    ecat_foe_soes_init(&adapter, &core, 512);

    foe_file_cfg_t file_cfg;
    memset(&file_cfg, 0, sizeof(file_cfg));

    uint8_t payload[16] = {1, 2, 3};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    /* 成功写入 (短包结束) */
    ecat_foe_soes_on_wrq(&adapter, "SERVO_DRV_MCU_V1.0.0.0.efw", 26);
    uint32_t ret_cb = ecat_foe_soes_write_cb(&file_cfg, efw, efw_len);
    TEST_ASSERT_EQ_INT(ret_cb, 0, "SOES 写入成功返回 0");
    TEST_ASSERT(!adapter.last_commit_failed, "last_commit_failed 必须为 false");

    /* 失败写入与 post_state_hook 拦截 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    ecat_foe_soes_init(&adapter, &core, 512);

    ecat_foe_soes_on_wrq(&adapter, "SERVO_DRV_MCU_V1.0.0.0.efw", 26);
    efw[63] ^= 0xFF; /* 破坏 CRC */
    ret_cb = ecat_foe_soes_write_cb(&file_cfg, efw, efw_len);
    TEST_ASSERT(ret_cb != 0, "失败回调返回非零");
    TEST_ASSERT(adapter.last_commit_failed, "last_commit_failed 为 true");

    uint8_t as = 0x13U; /* BOOT_TO_INIT */
    uint8_t an = 0x01U;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, 0x13U, "post hook 改写为 BOOT + Error (0x13)");
}

/* -------------------------------------------------------------------------- */
/*                     阶段 1 & 阶段 2 缺陷修复与回归测试套件                  */
/* -------------------------------------------------------------------------- */

/* 测项 18: 静态配置合法性校验与未配置核心拒绝 WRQ */
static void test_config_validation_rules(void)
{
    ecat_foe_core_t core;
    mock_driver_t mcu;
    mock_driver_init(&mcu);

    ecat_foe_core_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.project_id = 0x10010001U;
    cfg.project_name = "SERVO_DRV";
    cfg.hardware_id = 0x00000001U;
    cfg.local_hw_ver = 1U;
    cfg.max_header_size = 128U;
    cfg.targets[0].target_id = 1;
    cfg.targets[0].enabled = true;
    cfg.targets[0].max_payload_size = 384U * 1024U;
    cfg.targets[0].write_unit = 8U;
    cfg.targets[0].driver = &mcu.driver;
    cfg.targets[0].user_ctx = &mcu;

    /* A. 空指针配置 */
    TEST_ASSERT(ecat_foe_core_init(&core, NULL) != 0, "cfg 为 NULL 初始化必须报错");

    /* B. project_name 为空指针或空串或超长 */
    cfg.project_name = NULL;
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "project_name 为 NULL 必须报错");
    cfg.project_name = "";
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "project_name 为空字符串必须报错");
    cfg.project_name = "THIS_PROJECT_NAME_IS_TOO_LONG_EXCEEDING_31_BYTES";
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "project_name > 31 字符必须报错");
    cfg.project_name = "SERVO_DRV";

    /* C. max_header_size 下限/对齐/上限检查 */
    cfg.max_header_size = 60U; /* < 64 */
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "max_header_size < 64 必须报错");
    cfg.max_header_size = 65U; /* 非 4 对齐 */
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "max_header_size 非 4 字节对齐必须报错");
    cfg.max_header_size = 256U; /* > ECAT_FOE_HEADER_BUF_MAX */
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "max_header_size > 128 必须报错");
    cfg.max_header_size = 128U;

    /* D. 启用目标的驱动及回调完整性 */
    cfg.targets[0].driver = NULL;
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "启用目标 driver 为 NULL 必须报错");
    ecat_foe_target_driver_t bad_drv = mcu.driver;
    bad_drv.write = NULL;
    cfg.targets[0].driver = &bad_drv;
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "驱动缺少 write 回调必须报错");
    cfg.targets[0].driver = &mcu.driver;

    /* E. write_unit 边界 */
    cfg.targets[0].write_unit = 0;
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "write_unit == 0 必须报错");
    cfg.targets[0].write_unit = 512; /* > ECAT_FOE_MAX_WRITE_UNIT */
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "write_unit > 256 必须报错");
    cfg.targets[0].write_unit = 8U;

    /* F. 分区容量非零且为 write_unit 整数倍 */
    cfg.targets[0].max_payload_size = 0;
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "max_payload_size == 0 必须报错");
    cfg.targets[0].max_payload_size = 100U; /* 不是 8 的倍数 */
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "max_payload_size 不是 write_unit 整数倍必须报错");
    cfg.targets[0].max_payload_size = 384U * 1024U;

    /* G. 初始化成功与未配置核心拒绝 WRQ */
    TEST_ASSERT_EQ_INT(ecat_foe_core_init(&core, &cfg), 0, "合规配置初始化必须成功");
    TEST_ASSERT(core.is_configured, "is_configured 必须为 true");

    memset(&core, 0, sizeof(core)); /* 模拟未初始化的野核心 */
    int ret_wrq = ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw", 26);
    TEST_ASSERT(ret_wrq != 0, "未配置核心必须直接拒绝 WRQ");
}

/* 测项 19: 有界 WRQ 解析与文件名长度边界防御 */
static void test_bounded_wrq_parsing(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    /* 1. 长度为 0 */
    int ret0 = ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw", 0);
    TEST_ASSERT(ret0 != 0, "长度为 0 的 WRQ 必须拒绝");

    /* 2. 长度超过 FOE_FN_MAX (31) */
    int ret_long = ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0_OVERFLOW.efw", 35);
    TEST_ASSERT(ret_long != 0, "长度超过 31 的 WRQ 必须拒绝");

    /* 3. 无 \0 结尾的有界缓冲区解析 (末尾携带垃圾数据) */
    char raw_buf[64];
    memset(raw_buf, 'X', sizeof(raw_buf));
    memcpy(raw_buf, "SERVO_DRV_MCU_V1.0.0.0.efw", 26);
    /* 仅传入有效长度 26 字节，解析器不得向后读取 'X' */
    int ret_bound = ecat_foe_core_on_wrq(&core, raw_buf, 26);
    TEST_ASSERT_EQ_INT(ret_bound, 0, "显式传入 26 字节长度有界解析必须成功");
    TEST_ASSERT_EQ_INT(core.req_target_id, 1, "正确解析 target_id 为 1");
    TEST_ASSERT(core.req_fw_version == 0x01000000U, "正确解析版本");
}

/* 测项 20: 严格区分大小写 (.efw 小写、V 大写、目标名大写、项目名精确匹配) */
static void test_strict_case_sensitivity(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    /* 1. 后缀 .efw 必须小写 */
    TEST_ASSERT(test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.EFW") != 0, ".EFW 大写必须拒绝");
    TEST_ASSERT(test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.Efw") != 0, ".Efw 混写必须拒绝");
    TEST_ASSERT_EQ_INT(test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw"), 0, ".efw 小写必须通过");

    /* 2. 版本前缀 V 必须大写 */
    TEST_ASSERT(test_wrq(&core, "SERVO_DRV_MCU_v1.0.0.0.efw") != 0, "小写 v 前缀必须拒绝");
    TEST_ASSERT_EQ_INT(test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw"), 0, "大写 V 前缀必须通过");

    /* 3. 目标名必须全大写 */
    TEST_ASSERT(test_wrq(&core, "SERVO_DRV_mcu_V1.0.0.0.efw") != 0, "小写 mcu 必须拒绝");
    TEST_ASSERT(test_wrq(&core, "SERVO_DRV_Mcu_V1.0.0.0.efw") != 0, "首字母大写 Mcu 必须拒绝");

    /* 4. 项目名必须精确大小写匹配 */
    TEST_ASSERT(test_wrq(&core, "servo_drv_MCU_V1.0.0.0.efw") != 0, "项目名小写 servo_drv 必须拒绝");
}

/* 测项 21: 避免重复 abort (失败后新 WRQ 不再重复调用 abort) */
static void test_no_duplicate_abort_on_new_wrq(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    uint8_t payload[16] = {1, 2, 3};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    feed_stream_chunks(&core, efw, efw_len - 1, 64, false); /* 故意少发 1 字节导致失败 */
    int ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT(ret_end != 0, "少收结束必须失败");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "失败时已调用过一次 abort");

    /* 此时主站重新发起 WRQ，新会话自愈绝不重复调用 abort */
    int ret_new_wrq = test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_new_wrq, 0, "新 WRQ 必须通过");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "新 WRQ 绝不应重复调用 abort (保持为 1)");
}

/* 测项 22: 成功 finalize 后绝对不再调用 abort (即便后续更新下限失败) */
static void test_no_abort_after_finalize_success(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);
    core.cfg.anti_rollback_enable = true;

    /* 模拟持久化下限失败 */
    s_mock_set_floor_fail = true;

    uint8_t payload[16] = {1, 2, 3};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x02000000U, 0x01000000U, payload, 16);

    test_wrq(&core, "SERVO_DRV_MCU_V2.0.0.0.efw");
    feed_stream_chunks(&core, efw, efw_len, 64, false);
    int ret = ecat_foe_core_on_end(&core);
    TEST_ASSERT(ret != 0, "更新下限失败返回错误");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_ANTI_ROLLBACK_UPDATE, "报 ANTI_ROLLBACK_UPDATE");

    /* 固件已经成功 finalize，有效标记保持，绝不可调 abort */
    TEST_ASSERT(mcu.valid_flag, "固件必须保持有效状态");
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 1, "finalize 已执行成功");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "成功 finalize 后绝对不得调用 abort");

    /* 随后来新的 WRQ，也不得调用 abort */
    test_wrq(&core, "SERVO_DRV_MCU_V2.0.0.0.efw");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "后续新 WRQ 也不得调用 abort");
    s_mock_set_floor_fail = false;
}

/* 测项 23: 防回滚阶段 3 仅读取当前待升级目标 */
static void test_anti_rollback_only_reads_current_target(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);
    core.cfg.anti_rollback_enable = true;

    /* 注入故障：仅让 Target 2 (FPGA) 的 floor 获取失败 */
    s_mock_floor_fail_target_id = 2;

    /* 升级 Target 1 (MCU) */
    uint8_t payload[16] = {1, 2, 3};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x02000000U, 0x01000000U, payload, 16);

    test_wrq(&core, "SERVO_DRV_MCU_V2.0.0.0.efw");
    feed_stream_chunks(&core, efw, efw_len, 64, false);
    int ret = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret, 0, "Target 2 异常不影响 Target 1，仅查询当前升级目标");

    s_mock_floor_fail_target_id = 0;
}

/* 测项 24: 头部与多收同块时在 invalidate 前拒绝，保护旧有效标记 */
static void test_same_block_header_over_receive_protects_valid_flag(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    /* 模拟介质中原本已经存在有效固件 */
    mcu.valid_flag = true;

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");

    /* 构造一个首包，其头部声明 payload_length = 16 (总长 80)
     * 但本数据块推送了 64 字节头部 + 40 字节负载 = 104 字节 (同块多收溢出) */
    uint8_t big_chunk[128];
    uint8_t payload[40];
    memset(payload, 0x55, sizeof(payload));
    build_test_efw(big_chunk, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    /* 在末尾追加额外的多收字节，使单块总长达到 104 字节 */
    memset(big_chunk + 80, 0xAA, 24);

    /* 一次性将 104 字节推入核心 */
    int ret = ecat_foe_core_on_data(&core, big_chunk, 104);
    TEST_ASSERT(ret != 0, "同块多收超总长必须立即拒绝");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_OVER_RECEIVE, "报 OVER_RECEIVE 熔断");

    /* 核心断言：在 invalidate 之前即刻拒绝，旧有效标记绝对没有被破坏！ */
    TEST_ASSERT(mcu.valid_flag, "原有有效固件标记必须保持有效");
    TEST_ASSERT_EQ_INT(mcu.count_invalidate, 0, "绝对不得调用 invalidate()");
    TEST_ASSERT_EQ_INT(mcu.count_write, 0, "绝对不得向物理存储写入任何数据");
}

/* 测项 25: 驱动生命周期契约 (init 失败不调 abort；init 成功后 invalidate 失败必调 abort；新 WRQ 重试不重复 abort) */
static void test_driver_lifecycle_init_invalidate_abort_retry(void)
{
    mock_driver_t mcu;
    ecat_foe_core_t core;
    uint8_t payload[16] = {1, 2, 3};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    /* 1. init 失败：驱动自身未就绪，核心机不应调用 abort */
    mock_driver_init(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    mcu.inject_fail_init = true;
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    int ret = feed_stream_chunks(&core, efw, efw_len, 64, false);
    TEST_ASSERT(ret != 0, "init 失败必须拒绝");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_DRV_INIT, "报 DRV_INIT 错误");
    TEST_ASSERT_EQ_INT(mcu.count_init, 1, "init 被调用 1 次");
    TEST_ASSERT_EQ_INT(mcu.count_invalidate, 0, "init 失败不得调用 invalidate");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "init 自身失败核心机不调 abort");

    /* 2. init 成功但 invalidate 失败：生命周期已激活，必须调用 abort 释放资源 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    mcu.inject_fail_invalidate = true;
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    ret = feed_stream_chunks(&core, efw, efw_len, 64, false);
    TEST_ASSERT(ret != 0, "invalidate 失败必须拒绝");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_DRV_INVALIDATE, "报 DRV_INVALIDATE 错误");
    TEST_ASSERT_EQ_INT(mcu.count_init, 1, "init 成功调用 1 次");
    TEST_ASSERT_EQ_INT(mcu.count_invalidate, 1, "invalidate 被调用 1 次");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "invalidate 失败后必须调用 abort 释放资源");

    /* 3. 失败后主站发起新 WRQ 重试：自愈机制不得重复调用 abort */
    ret = test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret, 0, "新 WRQ 请求被正常接收");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "新 WRQ 自愈不应重复调用 abort (保持 1 次)");

    /* 4. 随后解除故障重试升级：完整流程必须成功完成且不再调用 abort */
    mcu.inject_fail_invalidate = false;
    ret = feed_stream_chunks(&core, efw, efw_len, 64, false);
    TEST_ASSERT_EQ_INT(ret, 0, "重试数据块传输成功");
    ret = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret, 0, "重试固件提交成功");
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 1, "重试 finalize 成功调用 1 次");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "整个重试成功流程未再调用 abort");
    TEST_ASSERT(mcu.valid_flag, "固件恢复有效标记");
}

/* -------------------------------------------------------------------------- */
/*                               主测试入口                                   */
/* -------------------------------------------------------------------------- */

int main(void)
{
    printf("====================================================\n");
    printf(" EtherCAT FoE 核心状态机与 SOES 适配层测试套件 (阶段 B)\n");
    printf(" 规范依据: SAD-v0.1 第 1、3、4 节\n");
    printf(" 编译标准: C99 (-std=c99 -Wall -Wextra -Werror -pedantic)\n");
    printf(" 动态内存: 零 malloc / 零动态分配\n");
    printf("====================================================\n");

    RUN_TEST(test_normal_upgrade_mcu);
    RUN_TEST(test_normal_upgrade_sii);
    RUN_TEST(test_normal_upgrade_eeprom);
    RUN_TEST(test_buffer_size_16_header_cross_chunks);
    RUN_TEST(test_buffer_size_32_header_cross_chunks);
    RUN_TEST(test_zero_length_chunk_and_exact_division);
    RUN_TEST(test_header_rejections_driver_zero_calls);
    RUN_TEST(test_filename_rejection_driver_zero_calls);
    RUN_TEST(test_call_sequence_and_mutex);
    RUN_TEST(test_stream_length_under_and_over_receive);
    RUN_TEST(test_crc_and_readback_errors);
    RUN_TEST(test_driver_operation_failures);
    RUN_TEST(test_on_abort_and_stack_abort_self_healing);
    RUN_TEST(test_capacity_boundary_exact_and_plus_one);
    RUN_TEST(test_write_unit_8_and_256_residual_and_readback_crc_excludes_padding);
    RUN_TEST(test_anti_rollback_full_behavior);
    RUN_TEST(test_soes_adapter_short_packet_and_post_hook);

    /* 阶段 1 & 阶段 2 修复回归测试项 */
    RUN_TEST(test_config_validation_rules);
    RUN_TEST(test_bounded_wrq_parsing);
    RUN_TEST(test_strict_case_sensitivity);
    RUN_TEST(test_no_duplicate_abort_on_new_wrq);
    RUN_TEST(test_no_abort_after_finalize_success);
    RUN_TEST(test_anti_rollback_only_reads_current_target);
    RUN_TEST(test_same_block_header_over_receive_protects_valid_flag);
    RUN_TEST(test_driver_lifecycle_init_invalidate_abort_retry);

    printf("\n核心测试运行统计: 共执行断言 %d 项 | 通过: %d 项 | 失败: %d 项\n",
           g_core_tests_run, g_core_tests_passed, g_core_tests_failed);

    if (g_core_tests_failed == 0) {
        printf(">>> 全部阶段 B 核心状态机测试通过！<<<\n");
        return 0;
    } else {
        printf(">>> 存在核心测试失败项！<<<\n");
        return 1;
    }
}
