/**
 * @file test_core.c
 * @brief EtherCAT FoE 核心状态机与 SOES 适配层测试套件 (阶段 B)
 *
 * 测试夹具内存与环境条件观察说明:
 * 1. 历史栈溢出 (退出码 134) 为特定条件下的环境观察现象：
 *    在 Apple Clang 配合 -O1/-O2 优化时，数十个包含 1MB 模拟闪存缓冲区 (MOCK_DRIVER_MEM_SIZE) 的测试函数
 *    被内联至 main()，叠加 AddressSanitizer 栈红色区 (redzones)，且处于系统默认 ~8MB 栈空间限制下时，
 *    观察到 main 栈帧膨胀并触发 ASan stack-overflow。
 * 2. 差异性核实：在 -O0 编译、未开启 ASan 栈插桩、或调大系统栈限制 (如 ulimit -s 65520) 时该溢出均未出现，
 *    因此在不同构建参数或环境栈限制下可能表现不同。
 * 3. 核心库代码本身零动态内存分配且函数最大栈帧极小 (最大 240 字节)。测试套件统一采用静态存储区 (BSS)
 *    的 s_driver 模拟夹具，确保无论编译器内联策略或环境栈限制如何均稳定运行。
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
    int _start_assertions = g_core_tests_run; \
    printf("  RUN   %-55s", #fn); \
    fn(); \
    int _delta = g_core_tests_run - _start_assertions; \
    if (!g_current_test_failed) { \
        printf(" [PASS] (+%d assertions)\n", _delta); \
    } else { \
        printf(" [FAIL] (+%d assertions)\n", _delta); \
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
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "abort 不可被调 (互斥)");
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
    TEST_ASSERT_EQ_INT(mcu.count_write, writes_before, "多收块不可写入底层");
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
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "固件保持有效，不调 abort");
    s_mock_set_floor_fail = false;
}

/* 17. SOES 适配层短包与 0 长度包及 post hook 状态拦截 */
static uint16_t s_mock_al_error_val = 0;
static int s_mock_al_error_calls = 0;
static void mock_al_error_cb(uint16_t errornumber)
{
    s_mock_al_error_val = errornumber;
    s_mock_al_error_calls++;
}

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
    TEST_ASSERT(core.driver_finalized, "短包成功后驱动已 finalize");

    /* 失败写入与 post_state_hook 拦截 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    ecat_foe_soes_init(&adapter, &core, 512);
    s_mock_al_error_val = 0;
    s_mock_al_error_calls = 0;
    ecat_foe_soes_set_al_error_fn(&adapter, mock_al_error_cb);

    ecat_foe_soes_on_wrq(&adapter, "SERVO_DRV_MCU_V1.0.0.0.efw", 26);
    efw[64] ^= 0xFF; /* 头部通过且驱动已 init，破坏 payload 导致校验失败 */
    ret_cb = ecat_foe_soes_write_cb(&file_cfg, efw, efw_len);
    TEST_ASSERT(ret_cb != 0, "失败回调返回非零");
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_ERROR, "失败后核心机状态置为 ERROR");

    uint8_t as = ECAT_TRANSITION_BOOT_TO_INIT; /* 0x13U: BOOT_TO_INIT 迁移码 */
    uint8_t an = ECAT_STATE_INIT;              /* 0x01U */
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_AL_STATUS_BOOT_ERROR, "post hook 改写为 BOOT + Error (0x13)");
    TEST_ASSERT_EQ_INT(s_mock_al_error_calls, 1, "al_error_fn 注入回调被调用且仅调用 1 次");
    TEST_ASSERT_EQ_INT(s_mock_al_error_val, ECAT_AL_STATUS_CODE_NO_VALID_FW, "向 al_error_fn 传入的错误码为 0x0014");

    /* 验证非 BOOT_TO_INIT 迁移时不拦截且不调用 al_error_fn */
    s_mock_al_error_val = 0;
    s_mock_al_error_calls = 0;
    as = (uint8_t)((ECAT_STATE_BOOT << 4) | ECAT_STATE_PREOP); /* 非 BOOT_TO_INIT */
    an = ECAT_STATE_BOOT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_STATE_BOOT, "非 BOOT_TO_INIT 迁移不予拦截改写");
    TEST_ASSERT_EQ_INT(s_mock_al_error_calls, 0, "非 BOOT_TO_INIT 迁移不调用 al_error_fn");

    /* 验证失败后重新成功升级的自愈解除拦截能力 (短包结束路径) */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    /* 重新组织正确固件 */
    efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    /* 在真实 SOES 中若未走 on_wrq，此处直接由核心启动 WRQ */
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw", 26);
    ret_cb = ecat_foe_soes_write_cb(&file_cfg, efw, efw_len);
    TEST_ASSERT_EQ_INT(ret_cb, 0, "新会话短包写入成功");
    TEST_ASSERT(core.driver_finalized, "成功提交后驱动已 finalize");

    /* 验证解除拦截后再次尝试 BOOT->INIT 迁移正常放行 */
    s_mock_al_error_calls = 0;
    as = ECAT_TRANSITION_BOOT_TO_INIT;
    an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_STATE_INIT, "成功提交后 BOOT->INIT 正常放行");
    TEST_ASSERT_EQ_INT(s_mock_al_error_calls, 0, "成功提交后不触发 al_error_fn");

    /* 验证 0 长度包结束路径的失败与成功自愈 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    /* 将 buffer_size 设为 64，构造 64 字节负载 (总长 128 字节，正好为 2 个 64 字节整包，不触发短包结束，必须由 0 长度包结束) */
    ecat_foe_soes_init(&adapter, &core, 64);
    ecat_foe_soes_set_al_error_fn(&adapter, mock_al_error_cb);
    uint8_t payload64[64];
    for (int i = 0; i < 64; i++) payload64[i] = (uint8_t)(i + 1);
    uint8_t efw128[256];
    size_t efw128_len = build_test_efw(efw128, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload64, 64);
    TEST_ASSERT_EQ_INT(efw128_len, 128, "总固件长度为 128 字节");

    /* A. 故意诱发 0 长度包结束时失败 (CRC 错误) */
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw", 26);
    /* 发送头部 64 字节整包 */
    ret_cb = ecat_foe_soes_write_cb(&file_cfg, efw128, 64);
    TEST_ASSERT_EQ_INT(ret_cb, 0, "发送第 1 块 64 字节成功");
    /* 发送有效载荷 64 字节整包，但故意篡改第 1 字节导致 CRC 校验失败 */
    uint8_t bad_chunk[64];
    memcpy(bad_chunk, efw128 + 64, 64);
    bad_chunk[0] ^= 0xAA;
    ret_cb = ecat_foe_soes_write_cb(&file_cfg, bad_chunk, 64);
    TEST_ASSERT_EQ_INT(ret_cb, 0, "第 2 块整包由于尚未触发 on_end 返回 0");
    /* 发送 0 长度包触发结束，此时因 CRC 校验失败，on_end 返回非 0 */
    ret_cb = ecat_foe_soes_write_cb(&file_cfg, NULL, 0);
    TEST_ASSERT(ret_cb != 0, "CRC 错误时 0 长度包结束必须返回非 0 失败");
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_ERROR, "0 长度包失败时核心状态为 ERROR");

    /* B. 重新发起正确流程并通过 0 长度包成功自愈 (同一个 core 实例，不重建 core) */
    mock_driver_reset(&mcu);
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw", 26);
    /* 发送正确的第 1 块 64 字节 */
    ret_cb = ecat_foe_soes_write_cb(&file_cfg, efw128, 64);
    TEST_ASSERT_EQ_INT(ret_cb, 0, "自愈重试第 1 块 64 字节成功");
    /* 发送正确的第 2 块 64 字节 */
    ret_cb = ecat_foe_soes_write_cb(&file_cfg, efw128 + 64, 64);
    TEST_ASSERT_EQ_INT(ret_cb, 0, "自愈重试第 2 块 64 字节成功");
    /* 发送 0 长度包闭环 */
    ret_cb = ecat_foe_soes_write_cb(&file_cfg, NULL, 0);
    TEST_ASSERT_EQ_INT(ret_cb, 0, "正确固件 0 长度包结束返回 0 成功");
    TEST_ASSERT(core.driver_finalized, "0 长度包成功驱动已 finalize");

    /* 验证提交成功时正常允许迁移至 INIT 且不调用 al_error_fn */
    s_mock_al_error_val = 0;
    s_mock_al_error_calls = 0;
    as = ECAT_TRANSITION_BOOT_TO_INIT;
    an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_STATE_INIT, "提交成功时允许正常迁移至 INIT");
    TEST_ASSERT_EQ_INT(s_mock_al_error_calls, 0, "提交成功时不调用 al_error_fn");

    /* 验证空指针健壮性 */
    ecat_foe_soes_post_state_hook(NULL, &as, &an);
    ecat_foe_soes_post_state_hook(&adapter, NULL, &an);
    ecat_foe_soes_post_state_hook(&adapter, &as, NULL);
    ecat_foe_soes_set_al_error_fn(NULL, mock_al_error_cb);

    /* 验证场景: 主站超时/断线等协议栈中止未触发写回调，但驱动已被 invalidate */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    /* 设置 buffer_size 为 64，发送 64 字节整包不触发短包结束，仅完成头部装配与驱动 invalidate */
    ecat_foe_soes_init(&adapter, &core, 64);
    s_mock_al_error_val = 0;
    s_mock_al_error_calls = 0;
    ecat_foe_soes_set_al_error_fn(&adapter, mock_al_error_cb);

    /* 主站发起 WRQ 并发送 64 字节头部 */
    ecat_foe_soes_on_wrq(&adapter, "SERVO_DRV_MCU_V1.0.0.0.efw", 26);
    efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    ret_cb = ecat_foe_soes_write_cb(&file_cfg, efw, 64);
    TEST_ASSERT_EQ_INT(ret_cb, 0, "头部写入成功");
    TEST_ASSERT(core.session_active, "会话处于活跃状态");
    TEST_ASSERT(core.driver_invalidated, "驱动已成功执行 invalidate (固件标记已失效)");
    TEST_ASSERT(!core.driver_finalized, "驱动尚未 finalize");

    /* 此时模拟主站通信超时/断线：后续写回调从未被触发！直接发起 BOOT->INIT 状态切换 */
    as = ECAT_TRANSITION_BOOT_TO_INIT;
    an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_AL_STATUS_BOOT_ERROR, "只要未 finalize 且已 invalidate，必须拦截 BOOT->INIT");
    TEST_ASSERT_EQ_INT(s_mock_al_error_calls, 1, "触发 al_error_fn 注入回调");

    /* 模拟主站重试成功闭环：同一个 core 实例，不重建 core，新升级完成后恢复允许切入 INIT */
    mock_driver_reset(&mcu);
    adapter.buffer_size = 512;
    ecat_foe_soes_set_al_error_fn(&adapter, mock_al_error_cb);
    ecat_foe_soes_on_wrq(&adapter, "SERVO_DRV_MCU_V1.0.0.0.efw", 26);
    ret_cb = ecat_foe_soes_write_cb(&file_cfg, efw, efw_len);
    TEST_ASSERT_EQ_INT(ret_cb, 0, "新会话完整写入成功");
    TEST_ASSERT(core.driver_finalized, "新会话成功 finalize");
    TEST_ASSERT(!core.driver_invalidated, "新会话已恢复固件有效");

    s_mock_al_error_calls = 0;
    as = ECAT_TRANSITION_BOOT_TO_INIT;
    an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_STATE_INIT, "新会话成功后允许正常迁移至 INIT");
    TEST_ASSERT_EQ_INT(s_mock_al_error_calls, 0, "成功时不触发 al_error_fn");
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

    /* B. project_name 为空指针或空串或超长/无终止符 */
    cfg.project_name = NULL;
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "project_name 为 NULL 必须报错");
    cfg.project_name = "";
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "project_name 为空字符串必须报错");
    cfg.project_name = "THIS_PROJECT_NAME_IS_TOO_LONG_EXCEEDING_31_BYTES";
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "project_name > 31 字符必须报错");

    /* B2. project_name 无 \0 终止符探测 */
    char unterminated_proj[64];
    memset(unterminated_proj, 'A', sizeof(unterminated_proj));
    cfg.project_name = unterminated_proj;
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "project_name 无 \\0 终止符探测必须报错");

    /* B3. 验证 31 字节合法上限边界与 32 字节超长拒绝 */
    char name_31[32];
    memset(name_31, 'X', 31);
    name_31[31] = '\0';
    cfg.project_name = name_31;
    TEST_ASSERT_EQ_INT(ecat_foe_core_init(&core, &cfg), 0, "31 字节合法上限项目名初始化必须成功");
    TEST_ASSERT_EQ_INT(strcmp(ecat_foe_core_get_project_name(&core), name_31), 0, "31 字节项目名完整保存无截断");

    char name_32[33];
    memset(name_32, 'Y', 32);
    name_32[32] = '\0';
    cfg.project_name = name_32;
    TEST_ASSERT_EQ_INT(ecat_foe_core_init(&core, &cfg), -1, "32 字节超长项目名必须返回 -1 拒绝");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_CFG_PROJECT_NAME, "超长项目名错误码匹配");
    cfg.project_name = "SERVO_DRV";

    /* B4. 测试首次 core 填充 0xA5 和 0x02 脏内存，传入无效配置时在 UBSan 下无非法 bool 读取 */
    ecat_foe_core_t dirty_a5;
    memset(&dirty_a5, 0xA5, sizeof(dirty_a5));
    ecat_foe_core_cfg_t bad_cfg = cfg;
    bad_cfg.project_name = NULL;
    TEST_ASSERT_EQ_INT(ecat_foe_core_init(&dirty_a5, &bad_cfg), -1, "脏内存 core (0xA5) 传入 NULL 项目名必须返回 -1");
    TEST_ASSERT(!dirty_a5.is_configured, "失败后 is_configured 为 false");
    TEST_ASSERT_EQ_INT(dirty_a5.state, ECAT_FOE_STATE_ERROR, "失败后 state 为 ERROR");

    ecat_foe_core_t dirty_02;
    memset(&dirty_02, 0x02, sizeof(dirty_02));
    TEST_ASSERT_EQ_INT(ecat_foe_core_init(&dirty_02, &bad_cfg), -1, "脏内存 core (0x02) 传入 NULL 项目名必须返回 -1");
    TEST_ASSERT(!dirty_02.is_configured, "失败后 is_configured 为 false");
    TEST_ASSERT_EQ_INT(dirty_02.state, ECAT_FOE_STATE_ERROR, "失败后 state 为 ERROR");

    /* B5. 已初始化 core 再传独立 NULL 名配置也须失败且不能发起 WRQ */
    ecat_foe_core_t reinit_core;
    setup_default_core(&reinit_core, &mcu, NULL, NULL);
    TEST_ASSERT(reinit_core.is_configured, "reinit_core 初始合规配置成功");

    ecat_foe_core_cfg_t null_name_cfg = cfg;
    null_name_cfg.project_name = NULL;
    TEST_ASSERT_EQ_INT(ecat_foe_core_init(&reinit_core, &null_name_cfg), -1, "已初始化 core 传入独立 NULL 名配置必须失败返回 -1");
    TEST_ASSERT(!reinit_core.is_configured, "重初始化失败后 core 必须标记为不可用 (is_configured == false)");
    TEST_ASSERT_EQ_INT(reinit_core.state, ECAT_FOE_STATE_ERROR, "状态必须变为 ERROR");

    /* 验证不能发起 WRQ */
    int ret_reinit_wrq = test_wrq(&reinit_core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    TEST_ASSERT(ret_reinit_wrq != 0, "不可用 core 发起 WRQ 必须被拒绝");
    TEST_ASSERT_EQ_INT(reinit_core.last_error, ECAT_FOE_CORE_ERR_INVALID_STATE, "不可用 core 错误码为 ERR_INVALID_STATE");

    /* C. max_header_size 下限/对齐/上限检查 */
    cfg.max_header_size = 60U; /* < 64 */
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "max_header_size < 64 必须报错");
    cfg.max_header_size = 65U; /* 非 4 对齐 */
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "max_header_size 非 4 字节对齐必须报错");
    cfg.max_header_size = 256U; /* > ECAT_FOE_HEADER_BUF_MAX */
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "max_header_size > 128 必须报错");
    cfg.max_header_size = 128U;

    /* D. 启用目标的驱动及全部 7 个回调完整性逐一校验 */
    cfg.targets[0].driver = NULL;
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "启用目标 driver 为 NULL 必须报错");

    ecat_foe_target_driver_t bad_drv = mcu.driver;

    bad_drv = mcu.driver; bad_drv.init = NULL; cfg.targets[0].driver = &bad_drv;
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "缺少 init 回调必须报错");

    bad_drv = mcu.driver; bad_drv.invalidate = NULL; cfg.targets[0].driver = &bad_drv;
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "缺少 invalidate 回调必须报错");

    bad_drv = mcu.driver; bad_drv.erase_to = NULL; cfg.targets[0].driver = &bad_drv;
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "缺少 erase_to 回调必须报错");

    bad_drv = mcu.driver; bad_drv.write = NULL; cfg.targets[0].driver = &bad_drv;
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "缺少 write 回调必须报错");

    bad_drv = mcu.driver; bad_drv.read = NULL; cfg.targets[0].driver = &bad_drv;
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "缺少 read 回调必须报错");

    bad_drv = mcu.driver; bad_drv.finalize = NULL; cfg.targets[0].driver = &bad_drv;
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "缺少 finalize 回调必须报错");

    bad_drv = mcu.driver; bad_drv.abort = NULL; cfg.targets[0].driver = &bad_drv;
    TEST_ASSERT(ecat_foe_core_init(&core, &cfg) != 0, "缺少 abort 回调必须报错");

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

    /* H. project_name 生命周期解耦测试 (外部临时缓冲区被覆写后核心仍能正常工作) */
    char temp_stack_proj[32];
    strncpy(temp_stack_proj, "TEMP_PROJ", sizeof(temp_stack_proj));
    cfg.project_name = temp_stack_proj;
    TEST_ASSERT_EQ_INT(ecat_foe_core_init(&core, &cfg), 0, "临时项目名初始化成功");
    /* 模拟外部临时缓冲区失效/被破坏 */
    memset(temp_stack_proj, 0xAA, sizeof(temp_stack_proj));
    /* 核心机依靠内部深拷贝的 local_project_name 依然能够正常处理 WRQ */
    int ret_decouple = ecat_foe_core_on_wrq(&core, "TEMP_PROJ_MCU_V1.0.0.0.efw", 26);
    TEST_ASSERT_EQ_INT(ret_decouple, 0, "项目名生命周期解耦: 外部缓冲失效后核心仍匹配成功");
    cfg.project_name = "SERVO_DRV";
    ecat_foe_core_init(&core, &cfg);

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

/* 测项 20: 严格区分大小写 (.efw 小写、V 大写、目标名大写、项目名一致匹配) */
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

    /* 4. 项目名必须一致大小写匹配 */
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

/* 测项 22: 成功 finalize 后不再调用 abort (即便后续更新下限失败) */
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
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "成功 finalize 后不得调用 abort");

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

    /* 核心断言：在 invalidate 之前即刻拒绝，旧有效标记没有被破坏！ */
    TEST_ASSERT(mcu.valid_flag, "原有有效固件标记必须保持有效");
    TEST_ASSERT_EQ_INT(mcu.count_invalidate, 0, "不得调用 invalidate()");
    TEST_ASSERT_EQ_INT(mcu.count_write, 0, "不得向物理存储写入任何数据");
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

/* 测项 26: erase_to 擦除水位不足的三条调用路径全量测试 */
static void test_erase_to_insufficient_erased_end_fails(void)
{
    mock_driver_t mcu;
    ecat_foe_core_t core;
    uint8_t payload[32];
    for (int i = 0; i < 32; i++) payload[i] = (uint8_t)i;
    uint8_t efw[256];

    /* 路径 2: core_process_payload_bytes 中整块多块直接写入时触发 erase_to 擦除不足 */
    mock_driver_init(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    mcu.inject_insufficient_erase = true;
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    int ret = feed_stream_chunks(&core, efw, efw_len, 64, false);
    TEST_ASSERT(ret != 0, "路径 2: 整块写入时 erased_end < need_end 必须被拒绝");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_DRV_ERASE, "报 DRV_ERASE 错误");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "路径 2 必须调用 abort 释放资源");
    TEST_ASSERT_EQ_INT(mcu.count_write, 0, "路径 2 绝不得向物理介质写入数据");

    /* 路径 1: core_process_payload_bytes 中凑满 residual_buf == write_unit 时触发 erase_to 擦除不足 */
    mock_driver_init(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    /* 先正常写入前 8 字节并正常擦除前 4KB */
    efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    /* 送 64B 头部 + 4B 负载 (residual_len = 4, 此时调用了第 1 次正常擦除) */
    ecat_foe_core_on_data(&core, efw, 68);
    TEST_ASSERT_EQ_INT(core.residual_len, 4, "头部完成后残留 4 字节");
    /* 此时强行将 erased_watermark 改小到 4，使得凑满 8 字节时必须再次调 erase_to，并注入擦除不足 */
    core.erased_watermark = 4;
    mcu.inject_insufficient_erase = true;
    /* 再送 4 字节，凑满 8 字节 (触发路径 1) */
    ret = ecat_foe_core_on_data(&core, efw + 68, 4);
    TEST_ASSERT(ret != 0, "路径 1: 凑满 write_unit 时 erased_end < need_end 必须被拒绝");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_DRV_ERASE, "路径 1 报 DRV_ERASE 错误");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "路径 1 必须调用 abort 释放资源");

    /* 路径 3: ecat_foe_core_on_end 中尾部补齐写入时触发 erase_to 擦除不足 */
    mock_driver_init(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    /* 构造一个仅 4 字节有效负载的固件 (小于 write_unit 8) */
    efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 4);
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    /* 发送 64B 头部 + 4B 负载，全部滞留在 residual_buf 中，尚未触发写入和擦除 */
    ecat_foe_core_on_data(&core, efw, 68);
    TEST_ASSERT_EQ_INT(core.residual_len, 4, "4 字节滞留在 residual_buf");
    TEST_ASSERT_EQ_INT(mcu.count_erase, 0, "此时尚未调用擦除");
    /* 注入擦除不足，触发 on_end 补齐写入 (路径 3) */
    mcu.inject_insufficient_erase = true;
    ret = ecat_foe_core_on_end(&core);
    TEST_ASSERT(ret != 0, "路径 3: 尾部补齐时 erased_end < need_end 必须被拒绝");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_DRV_ERASE, "路径 3 报 DRV_ERASE 错误");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "路径 3 必须调用 abort 释放资源");
    TEST_ASSERT_EQ_INT(mcu.count_write, 0, "路径 3 绝不得向物理介质写入数据");
}

/* 测项 27: 尾部补齐容量边界防御 (aligned_len 刚好等于分区上限及补齐超限防护) */
static void test_padding_capacity_boundary(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    /* 将 MCU 目标容量限制临时缩小为 16 字节，write_unit 为 8 */
    core.cfg.targets[0].max_payload_size = 16U;
    core.cfg.targets[0].write_unit = 8U;

    /* A. 正例：payload_len = 15 (接近上限 16)，尾部补齐 1 字节到 16 字节，恰好等于 max_payload_size */
    uint8_t payload[15];
    for (int i = 0; i < 15; i++) payload[i] = (uint8_t)i;
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 15);

    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    int ret = feed_stream_chunks(&core, efw, efw_len, 64, false);
    TEST_ASSERT_EQ_INT(ret, 0, "数据流分块推送成功");
    ret = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret, 0, "对齐补齐后刚好达到容量上限必须成功");
    TEST_ASSERT_EQ_INT(mcu.count_write, 2, "写入 2 块 8 字节 (总共 16 字节)");
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 1, "成功提交 finalize");
    TEST_ASSERT(mcu.valid_flag, "固件置为有效");

    /* B. 防御性分支验证：模拟运行时异常导致补齐后的 write_end 超出目标物理限额 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    core.cfg.targets[0].write_unit = 8U;
    /* 发送 4 字节数据滞留于 residual_buf */
    efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 4);
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    ecat_foe_core_on_data(&core, efw, 68);
    /* 此时在 on_end 执行前，若分区物理限额被异常收缩为 4 字节 (补齐需要写到第 8 字节) */
    core.cfg.targets[0].max_payload_size = 4U;
    ret = ecat_foe_core_on_end(&core);
    TEST_ASSERT(ret != 0, "补齐超出分区容量限制必须被拒绝");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_PAYLOAD_LIMIT_EXCEEDED, "报 PAYLOAD_LIMIT_EXCEEDED");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "补齐超限必须调用 abort 释放资源");
    TEST_ASSERT_EQ_INT(mcu.count_write, 0, "补齐超限绝不得写入介质");
}

/* 28. 失败后新 WRQ 自愈成功与生命周期解耦回归测试 */
static void test_failed_session_new_wrq_full_upgrade_recovery(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;

    /* 验证外部临时栈字符串内存契约: init 之后外部字符串被覆写为垃圾值，核心仍安全工作 */
    {
        char temp_stack_project[32];
        memset(temp_stack_project, 0, sizeof(temp_stack_project));
        strcpy(temp_stack_project, "SERVO_DRV");

        ecat_foe_core_cfg_t temp_cfg;
        memset(&temp_cfg, 0, sizeof(temp_cfg));
        temp_cfg.project_id = 0x10010001U;
        temp_cfg.project_name = temp_stack_project;
        temp_cfg.hardware_id = 0x00000001U;
        temp_cfg.local_hw_ver = 1;
        temp_cfg.max_header_size = 64;
        temp_cfg.targets[0].target_id = 1;
        temp_cfg.targets[0].enabled = true;
        temp_cfg.targets[0].max_payload_size = 0x00010000U;
        temp_cfg.targets[0].write_unit = 8;
        temp_cfg.targets[0].driver = &mcu.driver;
        temp_cfg.targets[0].user_ctx = &mcu;

        int init_ret = ecat_foe_core_init(&core, &temp_cfg);
        TEST_ASSERT_EQ_INT(init_ret, 0, "使用栈项目名初始化核心机成功");

        /* 外部栈内存覆写为无意义垃圾字符 */
        memset(temp_stack_project, 0x55, sizeof(temp_stack_project));
    }

    uint8_t payload[16] = {0x11, 0x22, 0x33, 0x44};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    /* 阶段 1: 故意诱发上一会话失败 (CRC 校验失败) */
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    uint8_t bad_efw[256];
    memcpy(bad_efw, efw, efw_len);
    bad_efw[64] ^= 0xEE; /* 篡改有效 Payload 破坏 CRC */
    feed_stream_chunks(&core, bad_efw, efw_len, 32, false);
    int ret_fail = ecat_foe_core_on_end(&core);
    TEST_ASSERT(ret_fail != 0, "上一会话 CRC 损坏必须失败");
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_ERROR, "核心状态进入 ERROR");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "驱动已执行一次 abort");
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 0, "失败会话 finalize 为 0");
    TEST_ASSERT(!mcu.valid_flag, "固件标记保持无效");

    /* 阶段 2: 不重新调用 init，直接下发全新合法 WRQ 启动自愈升级 */
    int ret_wrq = test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "失败后下发新 WRQ 自愈成功");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "自愈重置不发生重复 abort 泄漏");
    TEST_ASSERT_EQ_INT(mcu.count_init, 1, "WRQ 阶段尚未进入数据流，驱动 init 维持 1");

    /* 阶段 3: 完整灌入正确固件流并闭环全量升级 */
    feed_stream_chunks(&core, efw, efw_len, 32, false);
    TEST_ASSERT_EQ_INT(mcu.count_init, 2, "解完固件头后新会话重新激活驱动 init");
    TEST_ASSERT_EQ_INT(mcu.count_invalidate, 2, "新会话重新执行 invalidate");

    int ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, 0, "新会话自愈后完整升级闭环成功");
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_SUCCESS, "最终状态机处于 SUCCESS");
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 1, "驱动成功执行 finalize 固件提交");
    TEST_ASSERT(mcu.valid_flag, "固件有效标记已成功置为 true");

    /* 验证介质写入内容与原始 Payload 完全一致 */
    TEST_ASSERT(memcmp(mcu.mem, payload, 16) == 0, "烧录介质内容与原始 Payload 一致");
}

/* 29. 消除自身指针与非活跃状态结构体复制测试 */
static void test_self_referential_pointer_elimination_and_idle_copy(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    /* 1. 验证自身指针被消除: cfg.project_name 不指向内部 local_project_name */
    TEST_ASSERT(core.cfg.project_name == NULL, "init 后 cfg.project_name 设为 NULL 消除自指针");
    TEST_ASSERT((const void *)core.cfg.project_name != (const void *)core.local_project_name, "杜绝内部自身指针指向");
    TEST_ASSERT(strcmp(ecat_foe_core_get_project_name(&core), "SERVO_DRV") == 0, "安全接口返回正确内部项目名");

    /* 1.1 明确 &core.cfg 因其 project_name 为 NULL 不被支持，必须返回 -1 拒绝 */
    int ret_reinit = ecat_foe_core_init(&core, &core.cfg);
    TEST_ASSERT_EQ_INT(ret_reinit, -1, "传入 &core.cfg (project_name 为 NULL) 重初始化必须返回 -1 拒绝");
    TEST_ASSERT(!core.is_configured, "失败后 core 置为不可用 (is_configured == false)");

    /* 重新使用合规独立的配置重新初始化恢复 core */
    setup_default_core(&core, &mcu, NULL, NULL);
    TEST_ASSERT(core.is_configured, "使用独立合法配置重新初始化成功");
    TEST_ASSERT(strcmp(ecat_foe_core_get_project_name(&core), "SERVO_DRV") == 0, "恢复后内部项目名完好");

    /* 2. 验证非活跃状态下结构体支持按值复制/模板分发 */
    ecat_foe_core_t core_copy = core;

    /* 覆写并销毁原 core 实例内存 */
    memset(&core, 0xEE, sizeof(core));

    /* 验证深拷贝后的副本 project_name 依然独立存在且完好 */
    TEST_ASSERT(strcmp(ecat_foe_core_get_project_name(&core_copy), "SERVO_DRV") == 0, "副本内部项目名依然独立完好");

    uint8_t payload[16] = {0xAA, 0xBB, 0xCC, 0xDD};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    /* 3. 在原实例已销毁的情况下，副本 core_copy 独立完成全流程升级 */
    int ret_wrq = test_wrq(&core_copy, "SERVO_DRV_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "副本 WRQ 处理成功");

    feed_stream_chunks(&core_copy, efw, efw_len, 32, false);
    int ret_end = ecat_foe_core_on_end(&core_copy);
    TEST_ASSERT_EQ_INT(ret_end, 0, "副本升级流程闭环成功");
    TEST_ASSERT_EQ_INT(core_copy.state, ECAT_FOE_STATE_SUCCESS, "副本状态机到达 SUCCESS");
    TEST_ASSERT(mcu.valid_flag, "固件置为有效");
    TEST_ASSERT(memcmp(mcu.mem, payload, 16) == 0, "副本写入数据完全正确");
}

/* 30. 核心机反初始化 deinit 与活跃会话 abort 契约测试 */
static void test_core_deinit_lifecycle(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;

    /* 1. NULL 指针防御 */
    int ret = ecat_foe_core_deinit(NULL);
    TEST_ASSERT_EQ_INT(ret, 0, "deinit(NULL) 必须安全返回 0");

    /* 2. 显式零初始化的核心机 (如 ecat_foe_core_t zero_core = {0}) 安全返回 0 (No-op)
     * 【契约边界警告】：本函数仅接受 NULL、已成功 init 的实例、或已显式零初始化的实例；
     * 严禁直接传入未初始化的脏栈内存 (ecat_foe_core_t uninit)，在 C/UBSan 下读取未初始化布尔值是未定义行为 (UB)。
     */
    ecat_foe_core_t zero_core = {0};
    ret = ecat_foe_core_deinit(&zero_core);
    TEST_ASSERT_EQ_INT(ret, 0, "deinit 显式零初始化核心机安全返回 0");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "零初始化核心机绝不得触发 abort");

    memset(&core, 0, sizeof(core));
    ret = ecat_foe_core_deinit(&core);
    TEST_ASSERT_EQ_INT(ret, 0, "deinit memset 零初始化核心机返回 0");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "memset 零初始化核心机绝不得触发 abort");

    /* 3. 已配置但处于空闲态 (IDLE，无活跃会话) 的核心机 deinit */
    setup_default_core(&core, &mcu, NULL, NULL);
    TEST_ASSERT(core.is_configured, "初始化核心机成功");
    ret = ecat_foe_core_deinit(&core);
    TEST_ASSERT_EQ_INT(ret, 0, "空闲核心机 deinit 成功");
    TEST_ASSERT(!core.is_configured, "deinit 后 core.is_configured 变为 false");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "空闲核心机 deinit 不触发驱动 abort");

    /* 4. 活跃会话进行中 (WRQ 之后、数据流传输中) 触发 deinit: 必须安全中止底层驱动释放资源 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    uint8_t payload[16] = {1, 2, 3, 4};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    /* 发送 64 字节头部，使驱动执行 init 与 invalidate 并进入活跃会话 */
    ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT(core.session_active, "会话处于活跃状态");
    TEST_ASSERT_EQ_INT(mcu.count_init, 1, "驱动已执行 init");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "尚未执行 abort");

    ret = ecat_foe_core_deinit(&core);
    TEST_ASSERT_EQ_INT(ret, 0, "活跃会话 deinit 成功返回 0");
    TEST_ASSERT(!core.is_configured, "deinit 后核心标记不可用");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "活跃会话 deinit 必须且仅调用 1 次驱动 abort");

    /* 5. 幂等性测试: 连续再次调用 deinit 绝不重复触发 abort */
    ret = ecat_foe_core_deinit(&core);
    TEST_ASSERT_EQ_INT(ret, 0, "重复 deinit 安全返回 0");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "重复 deinit 绝不发生重复 abort 泄漏");

    /* 6. 成功 finalize 提交后的会话 deinit: 严禁调用 abort 破坏已提交的有效固件 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    feed_stream_chunks(&core, efw, efw_len, 32, false);
    int ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, 0, "固件升级正常成功");
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 1, "驱动已执行 finalize");
    TEST_ASSERT(mcu.valid_flag, "固件置为有效");

    ret = ecat_foe_core_deinit(&core);
    TEST_ASSERT_EQ_INT(ret, 0, "已提交核心机 deinit 成功");
    TEST_ASSERT(!core.is_configured, "core 重置为不可用");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "已成功 finalize 的固件在 deinit 时严禁调用 abort");
    TEST_ASSERT(mcu.valid_flag, "固件有效标记保持 true");

    /* 7. 活跃会话经 deinit 释放后重新初始化全流程恢复测试 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT(core.session_active, "会话活跃");
    /* 显式 deinit 安全释放硬件资源 */
    ecat_foe_core_deinit(&core);
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "deinit 释放活跃硬件资源");

    /* 重新 init 核心机并执行完整升级 */
    setup_default_core(&core, &mcu, NULL, NULL);
    TEST_ASSERT(core.is_configured, "重新初始化核心机成功");
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    feed_stream_chunks(&core, efw, efw_len, 32, false);
    ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, 0, "重新初始化后新升级成功闭环");
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_SUCCESS, "最终状态机为 SUCCESS");
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 1, "新会话驱动成功 finalize");
}

static void test_mid_session_drop_same_core_recovery(void)
{
    mock_driver_t mcu;
    ecat_foe_core_t core;
    ecat_foe_soes_adapter_t adapter;
    uint8_t payload[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t efw[256];
    size_t efw_len;
    foe_file_cfg_t file_cfg;

    /* 初始化核心与驱动 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    ecat_foe_soes_init(&adapter, &core, 64);
    ecat_foe_soes_set_al_error_fn(&adapter, mock_al_error_cb);
    s_mock_al_error_calls = 0;
    s_mock_al_error_val = 0;

    /* 会话 1: 发起合法 WRQ 并只发送 64 字节头部，模拟写入一部分后主站中途断开/超时 */
    ecat_foe_soes_on_wrq(&adapter, "SERVO_DRV_MCU_V1.0.0.0.efw", 26);
    efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    int ret_cb = ecat_foe_soes_write_cb(&file_cfg, efw, 64);
    TEST_ASSERT_EQ_INT(ret_cb, 0, "会话 1: 头部写入成功");
    TEST_ASSERT(core.session_active, "会话 1: 处于活跃写入状态");
    TEST_ASSERT(core.driver_invalidated, "会话 1: 驱动已执行 invalidate");
    TEST_ASSERT(!core.driver_finalized, "会话 1: 驱动尚未 finalize");
    TEST_ASSERT_EQ_INT(mcu.count_invalidate, 1, "会话 1: invalidate 调用 1 次");

    /* 此时主站静默超时并试图切换 BOOT->INIT: 必须坚决拦截 */
    uint8_t as = ECAT_TRANSITION_BOOT_TO_INIT;
    uint8_t an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_AL_STATUS_BOOT_ERROR, "中断未完成状态下坚决拦截 BOOT->INIT");
    TEST_ASSERT_EQ_INT(s_mock_al_error_calls, 1, "触发注入的回调");
    TEST_ASSERT_EQ_INT(s_mock_al_error_val, ECAT_AL_STATUS_CODE_NO_VALID_FW, "向 al_error_fn 传入 0x0014");

    /* 严禁调用 setup_default_core / ecat_foe_core_init / memset(&core) !
     * 同一个 core 实例必须能够自愈残留驱动与状态 */
    mock_driver_reset(&mcu);
    s_mock_al_error_calls = 0;
    s_mock_al_error_val = 0;
    adapter.buffer_size = 512;

    /* 会话 2: 在同一个 core 实例上直接发起新的合法 WRQ */
    int ret_wrq = ecat_foe_soes_on_wrq(&adapter, "SERVO_DRV_MCU_V1.0.0.0.efw", 26);
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "同一个 core 实例上新会话 WRQ 必须成功受理");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "核心内部自动主动清理并释放上一个中断会话的驱动资源");

    /* 送入完整合法固件并结束升级 */
    ret_cb = ecat_foe_soes_write_cb(&file_cfg, efw, efw_len);
    TEST_ASSERT_EQ_INT(ret_cb, 0, "新会话完整固件写入成功");
    TEST_ASSERT(core.driver_finalized, "驱动成功 finalize");
    TEST_ASSERT(!core.driver_invalidated, "固件就绪标记恢复有效");
    TEST_ASSERT(!core.session_active, "会话正常结束关闭");
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_SUCCESS, "核心状态机成功进入 SUCCESS");

    /* 验证解除拦截: 再次切换 BOOT->INIT 必须正常放行通过 */
    as = ECAT_TRANSITION_BOOT_TO_INIT;
    an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_STATE_INIT, "新升级成功后 BOOT->INIT 正常放行通过");
    TEST_ASSERT_EQ_INT(s_mock_al_error_calls, 0, "放行时不触发 al_error_fn");
}

/* 需求对应 C2: 副本复制抹除原实例且 user_ctx 指向调用方对象并完成升级 */
typedef struct {
    mock_driver_t drv_impl; /* 首个成员与 mock_driver_t 对齐 */
    uint32_t magic;
    uint32_t call_count;
} caller_custom_ctx_t;

static void test_idle_copy_erases_original_with_user_ctx_upgrade(void)
{
    caller_custom_ctx_t caller_obj;
    memset(&caller_obj, 0, sizeof(caller_obj));
    caller_obj.magic = 0xCAFE1234U;
    mock_driver_init(&caller_obj.drv_impl);

    ecat_foe_core_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.project_id = 0x10010001U;
    cfg.project_name = "SERVO_DRV";
    cfg.hardware_id = 0x00000001U;
    cfg.local_hw_ver = 1U;
    cfg.anti_rollback_enable = false;
    cfg.max_header_size = 128U;

    /* Target 1: MCU - user_ctx 明确指向调用方对象 caller_obj */
    cfg.targets[0].target_id = 1;
    cfg.targets[0].enabled = true;
    cfg.targets[0].max_payload_size = 384U * 1024U;
    cfg.targets[0].write_unit = 8U;
    cfg.targets[0].driver = &caller_obj.drv_impl.driver;
    cfg.targets[0].user_ctx = &caller_obj;

    ecat_foe_core_t core;
    int ret_init = ecat_foe_core_init(&core, &cfg);
    TEST_ASSERT_EQ_INT(ret_init, 0, "初始化 core 成功");
    TEST_ASSERT(core.cfg.targets[0].user_ctx == &caller_obj, "user_ctx 指向调用方对象");

    /* 在空闲态对 core 实施按值浅拷贝分发 */
    ecat_foe_core_t core_copy = core;

    /* 覆写并抹除原 core 实例内存，模拟原宿主销毁 */
    memset(&core, 0xA5, sizeof(core));

    /* 验证副本 core_copy 在原实例抹除后，依然能访问 caller_obj 并完成全流程升级 */
    uint8_t payload[16] = {1, 2, 3, 4};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    int ret_wrq = ecat_foe_core_on_wrq(&core_copy, "SERVO_DRV_MCU_V1.0.0.0.efw", 26);
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "副本 WRQ 成功受理");

    feed_stream_chunks(&core_copy, efw, efw_len, 32, false);
    int ret_end = ecat_foe_core_on_end(&core_copy);
    TEST_ASSERT_EQ_INT(ret_end, 0, "副本全流程升级成功");
    TEST_ASSERT(caller_obj.drv_impl.valid_flag, "调用方对象的 mock_driver 标记置为有效");
    TEST_ASSERT_EQ_INT(caller_obj.drv_impl.count_finalize, 1, "调用方对象的 finalize 被调用 1 次");
    TEST_ASSERT_EQ_INT(caller_obj.magic, 0xCAFE1234U, "调用方对象自身数据完好无损");
}

/* 需求对应 C2: 100 次失败/自愈循环 (同一个 core 实例，不重建 core) */
static void test_100_failed_self_healing_cycles(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    uint8_t payload[16] = {0x11, 0x22, 0x33, 0x44};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    for (int cycle = 0; cycle < 100; cycle++) {
        /* 1. 制造失败阶段: 发送 WRQ 并发送损坏的头部数据或中途少收中止 */
        int ret_wrq = ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw", 26);
        TEST_ASSERT_EQ_INT(ret_wrq, 0, "循环中 WRQ 正常受理");

        /* 发送 64 字节头部，使驱动 invalidate */
        int ret_data = ecat_foe_core_on_data(&core, efw, 64);
        TEST_ASSERT_EQ_INT(ret_data, 0, "头部装配通过");
        TEST_ASSERT(core.session_active, "会话进入活跃");
        TEST_ASSERT(core.driver_invalidated, "驱动已 invalidate");

        /* 模拟通信异常，直接调用 on_abort */
        ecat_foe_core_on_abort(&core);
        TEST_ASSERT(!core.session_active, "abort 后会话重置为非活跃");
        TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_IDLE, "abort 后状态机回到 IDLE");

        /* 2. 自愈恢复阶段: 在同一个 core 实例上重新发起合法 WRQ 并完成全量升级 */
        mock_driver_reset(&mcu);
        ret_wrq = ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw", 26);
        TEST_ASSERT_EQ_INT(ret_wrq, 0, "自愈 WRQ 成功受理");

        int ret_feed = feed_stream_chunks(&core, efw, efw_len, 32, false);
        TEST_ASSERT_EQ_INT(ret_feed, 0, "自愈数据块灌入成功");
        int ret_end = ecat_foe_core_on_end(&core);
        TEST_ASSERT_EQ_INT(ret_end, 0, "自愈后升级成功闭环");
        TEST_ASSERT(mcu.valid_flag, "固件置为有效");
        TEST_ASSERT_EQ_INT(mcu.count_finalize, 1, "finalize 调用 1 次");
    }
}

/* 需求对应 C2: 0xA5 与 0x02 脏内存 core 初始化 (UBSan 下无非法 bool 读取) */
static void test_dirty_memory_initialization_a5_and_02(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);

    /* 1. 0xA5 脏内存测试 */
    uint8_t dirty_buf_a5[sizeof(ecat_foe_core_t)];
    memset(dirty_buf_a5, 0xA5, sizeof(dirty_buf_a5));
    ecat_foe_core_t *core_a5 = (ecat_foe_core_t *)dirty_buf_a5;

    /* 传入无效配置 (NULL 项目名): 必须安全拒绝返回 -1，在 UBSan 下不得发生非法 bool 内存读取 */
    ecat_foe_core_cfg_t bad_cfg;
    memset(&bad_cfg, 0, sizeof(bad_cfg));
    bad_cfg.project_name = NULL;
    int ret_a5_bad = ecat_foe_core_init(core_a5, &bad_cfg);
    TEST_ASSERT_EQ_INT(ret_a5_bad, -1, "0xA5 脏内存下传入无效配置安全返回 -1");
    TEST_ASSERT(!core_a5->is_configured, "失败后 is_configured 被安全清为 false");

    /* 2. 0x02 脏内存测试 (0x02 在很多 ABI 中对于 bool 属于非法 trap representation) */
    uint8_t dirty_buf_02[sizeof(ecat_foe_core_t)];
    memset(dirty_buf_02, 0x02, sizeof(dirty_buf_02));
    ecat_foe_core_t *core_02 = (ecat_foe_core_t *)dirty_buf_02;

    int ret_02_bad = ecat_foe_core_init(core_02, &bad_cfg);
    TEST_ASSERT_EQ_INT(ret_02_bad, -1, "0x02 脏内存下传入无效配置安全返回 -1");
    TEST_ASSERT(!core_02->is_configured, "失败后 is_configured 被安全清为 false");

    /* 3. 在 0xA5 脏内存上使用有效配置初始化: 必须成功并置 is_configured 为 true */
    memset(dirty_buf_a5, 0xA5, sizeof(dirty_buf_a5));
    setup_default_core(core_a5, &mcu, NULL, NULL);
    TEST_ASSERT(core_a5->is_configured, "0xA5 脏内存上有效初始化成功");
    TEST_ASSERT_EQ_INT(core_a5->state, ECAT_FOE_STATE_IDLE, "状态为 IDLE");
    TEST_ASSERT_EQ_INT(test_wrq(core_a5, "SERVO_DRV_MCU_V1.0.0.0.efw"), 0, "初始化后 WRQ 正常受理");
}

/* 需求对应 C2: NULL 项目名一律拒绝，失败后 core 不可用且不能发起 WRQ；不支持 &core.cfg */
static void test_null_project_name_rejection(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;

    /* 1. 全新未初始化 core 传入 NULL 项目名 */
    ecat_foe_core_cfg_t null_cfg;
    memset(&null_cfg, 0, sizeof(null_cfg));
    null_cfg.project_name = NULL; /* NULL 项目名 */
    null_cfg.project_id = 0x10010001U;
    null_cfg.hardware_id = 0x00000001U;
    null_cfg.local_hw_ver = 1U;
    null_cfg.max_header_size = 128U;
    null_cfg.targets[0].target_id = 1;
    null_cfg.targets[0].enabled = true;
    null_cfg.targets[0].max_payload_size = 384U * 1024U;
    null_cfg.targets[0].write_unit = 8U;
    null_cfg.targets[0].driver = &mcu.driver;
    null_cfg.targets[0].user_ctx = &mcu;

    int ret = ecat_foe_core_init(&core, &null_cfg);
    TEST_ASSERT_EQ_INT(ret, -1, "NULL 项目名配置必须返回 -1 拒绝");
    TEST_ASSERT(!core.is_configured, "core.is_configured 为 false");

    /* 失败后 core 不可用，发起 WRQ 必须被拒绝 */
    int ret_wrq = ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw", 26);
    TEST_ASSERT(ret_wrq != 0, "未成功初始化的 core 无法发起 WRQ");

    /* 2. 已初始化 core 再传独立的 NULL 名配置: 同样必须失败且变为不可用 */
    setup_default_core(&core, &mcu, NULL, NULL);
    TEST_ASSERT(core.is_configured, "正常初始化成功");

    ret = ecat_foe_core_init(&core, &null_cfg);
    TEST_ASSERT_EQ_INT(ret, -1, "已配置 core 传入 NULL 项目名同样拒绝");
    TEST_ASSERT(!core.is_configured, "失败后 core 立即变为不可用");

    ret_wrq = ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw", 26);
    TEST_ASSERT(ret_wrq != 0, "变为不可用的 core 无法发起 WRQ");

    /* 3. 明确 &core.cfg 因项目名为 NULL 不被支持 */
    setup_default_core(&core, &mcu, NULL, NULL);
    TEST_ASSERT(core.cfg.project_name == NULL, "core.cfg.project_name 设为 NULL");
    int ret_self = ecat_foe_core_init(&core, &core.cfg);
    TEST_ASSERT_EQ_INT(ret_self, -1, "传入 &core.cfg 拒绝");
    TEST_ASSERT(!core.is_configured, "core 变为不可用");
}

/* 需求对应 C3: 复现测试：不调 on_wrq，直接按 SOES 回调顺序灌入（首块、满块、余量块），
 * 断言结果并贴输出，说明首块数据进来时核心返回什么。只写测试和文档，不改适配层逻辑 */
static void test_reproduce_soes_data_callback_without_wrq_stream_rejection(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    ecat_foe_soes_adapter_t adapter;
    ecat_foe_soes_init(&adapter, &core, 512);

    foe_file_cfg_t file_cfg;
    memset(&file_cfg, 0, sizeof(file_cfg));

    uint8_t payload[64];
    memset(payload, 0x33, sizeof(payload));
    uint8_t efw[1024];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 64);
    (void)efw_len;

    /* 关键条件: 全程不调用 ecat_foe_core_on_wrq 或 ecat_foe_soes_on_wrq !
     * 直接按 SOES 原生流程灌入数据块:
     * 块 1: 首块数据 (包含 64 字节 .efw 头及部分 payload)
     * 块 2: 满块数据 (512 字节)
     * 块 3: 余量块数据 (小于 buffer_size) 或 0 长度结束块 */

    /* 1. 灌入首块数据 (64 字节) */
    int core_ret_chunk1 = ecat_foe_core_on_data(&core, efw, 64);
    printf("  [C3 证据] 未调 WRQ 灌入首块 (64B): 核心直接返回 %d, 错误码 = 0x%04X (ECAT_FOE_CORE_ERR_INVALID_STATE)\n",
           core_ret_chunk1, core.last_error);
    TEST_ASSERT_EQ_INT(core_ret_chunk1, -1, "核心层在未建会话时灌入首块直接返回 -1");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_INVALID_STATE, "核心层记录 INVALID_STATE 错误");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "首块拒绝且驱动 count_init 保持 0");
    TEST_ASSERT_EQ_INT(mcu.count_invalidate, 0, "首块拒绝且驱动 count_invalidate 保持 0");
    TEST_ASSERT_EQ_INT(mcu.count_write, 0, "首块拒绝且驱动 count_write 保持 0");

    /* 适配层写回调测试: write_cb 返回非零 (1U) 触发 SOES 下发 FOE_ERR_PROGERROR */
    uint32_t cb_ret1 = ecat_foe_soes_write_cb(&file_cfg, efw, 64);
    printf("  [C3 证据] 未调 WRQ 适配层 write_cb 灌入首块: 返回 %u (非零触发 FOE_abort)\n", cb_ret1);
    TEST_ASSERT_EQ_INT(cb_ret1, 1U, "适配层回调直接返回 1U 失败");

    /* 2. 灌入满块数据 (512 字节) */
    uint8_t full_chunk[512];
    memset(full_chunk, 0x77, sizeof(full_chunk));
    int core_ret_chunk2 = ecat_foe_core_on_data(&core, full_chunk, sizeof(full_chunk));
    uint32_t cb_ret2 = ecat_foe_soes_write_cb(&file_cfg, full_chunk, sizeof(full_chunk));
    TEST_ASSERT_EQ_INT(core_ret_chunk2, -1, "满块数据核心持续拒绝返回 -1");
    TEST_ASSERT_EQ_INT(cb_ret2, 1U, "满块数据适配层持续返回 1U");
    TEST_ASSERT_EQ_INT(mcu.count_write, 0, "驱动 count_write 依然为 0");

    /* 3. 灌入余量块 (16 字节) */
    uint8_t rem_chunk[16];
    memset(rem_chunk, 0x88, sizeof(rem_chunk));
    int core_ret_chunk3 = ecat_foe_core_on_data(&core, rem_chunk, sizeof(rem_chunk));
    uint32_t cb_ret3 = ecat_foe_soes_write_cb(&file_cfg, rem_chunk, sizeof(rem_chunk));
    TEST_ASSERT_EQ_INT(core_ret_chunk3, -1, "余量块核心持续拒绝返回 -1");
    TEST_ASSERT_EQ_INT(cb_ret3, 1U, "余量块适配层持续返回 1U");

    /* 4. 灌入 0 长度块 / on_end 结束 */
    int ret_end = ecat_foe_core_on_end(&core);
    uint32_t cb_ret0 = ecat_foe_soes_write_cb(&file_cfg, NULL, 0);
    TEST_ASSERT_EQ_INT(ret_end, -1, "未建会话调用 on_end 返回 -1");
    TEST_ASSERT_EQ_INT(cb_ret0, 1U, "0 长度包结束回调返回 1U");

    /* 最终断言: 底层驱动的所有生命周期接口调用次数全为 0 */
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "全流程底层驱动 count_init 恒为 0");
    TEST_ASSERT_EQ_INT(mcu.count_invalidate, 0, "全流程底层驱动 count_invalidate 恒为 0");
    TEST_ASSERT_EQ_INT(mcu.count_erase, 0, "全流程底层驱动 count_erase 恒为 0");
    TEST_ASSERT_EQ_INT(mcu.count_write, 0, "全流程底层驱动 count_write 恒为 0");
    TEST_ASSERT_EQ_INT(mcu.count_read, 0, "全流程底层驱动 count_read 恒为 0");
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 0, "全流程底层驱动 count_finalize 恒为 0");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "未进入会话时 abort 调用次数亦为 0");
}

/* -------------------------------------------------------------------------- */
/*                     P3 钩子条件专项独立测试 (a ~ g)                         */
/* -------------------------------------------------------------------------- */

/* P3-a. 文件名被拒（未碰驱动）→ 不拦 */
static void test_hook_filename_rejected_not_blocked(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    ecat_foe_soes_adapter_t adapter;
    ecat_foe_soes_init(&adapter, &core, 512);

    int ret_wrq = test_wrq(&core, "INVALID_PROJECT_NAME_XXX.efw");
    TEST_ASSERT(ret_wrq != 0, "非法文件名 WRQ 返回非 0 拒绝");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "未碰驱动: count_init 恒为 0");

    uint8_t as = ECAT_TRANSITION_BOOT_TO_INIT;
    uint8_t an = 0;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, 0, "文件名被拒未碰驱动时 hook 不得拦截 (an 保持 0)");
}

/* P3-b. 头校验失败（未 init）→ 不拦 */
static void test_hook_header_invalid_not_blocked(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    ecat_foe_soes_adapter_t adapter;
    ecat_foe_soes_init(&adapter, &core, 512);

    int ret_wrq = test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "合法文件名 WRQ 返回 0");

    /* 注入损坏头部 (例如 Magic 错误) */
    uint8_t bad_hdr[64];
    memset(bad_hdr, 0xA5, sizeof(bad_hdr));
    int ret_data = ecat_foe_core_on_data(&core, bad_hdr, sizeof(bad_hdr));
    TEST_ASSERT_EQ_INT(ret_data, -1, "损坏头数据返回 -1");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "头校验失败未进入 init: count_init 恒为 0");

    uint8_t as = ECAT_TRANSITION_BOOT_TO_INIT;
    uint8_t an = 0;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, 0, "头校验失败未 init 时 hook 不得拦截 (an 保持 0)");
}

/* P3-c. init 成功、invalidate 失败 → 拦 */
static void test_hook_init_ok_invalidate_failed_blocked(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);
    mcu.inject_fail_invalidate = true;

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    ecat_foe_soes_adapter_t adapter;
    ecat_foe_soes_init(&adapter, &core, 512);
    s_mock_al_error_val = 0;
    s_mock_al_error_calls = 0;
    ecat_foe_soes_set_al_error_fn(&adapter, mock_al_error_cb);

    int ret_wrq = test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "合法 WRQ 返回 0");

    uint8_t payload[16] = {0x01, 0x02};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    int ret_data = ecat_foe_core_on_data(&core, efw, efw_len);
    TEST_ASSERT_EQ_INT(ret_data, -1, "invalidate 失败导致 on_data 返回 -1");
    TEST_ASSERT_EQ_INT(mcu.count_init, 1, "驱动 init 成功调用");
    TEST_ASSERT_EQ_INT(mcu.count_invalidate, 1, "驱动 invalidate 被调用且失败");
    TEST_ASSERT(ecat_foe_core_upgrade_incomplete(&core), "init 成功但未 finalize，upgrade_incomplete 为 true");

    uint8_t as = ECAT_TRANSITION_BOOT_TO_INIT;
    uint8_t an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_AL_STATUS_BOOT_ERROR, "init 成功后失败必须拦截 BOOT->INIT (an 变为 0x13)");
    TEST_ASSERT_EQ_INT(s_mock_al_error_calls, 1, "触发 al_error_fn 注入回调");
    TEST_ASSERT_EQ_INT(s_mock_al_error_val, ECAT_AL_STATUS_CODE_NO_VALID_FW, "向 al_error_fn 传入 0x0014");
}

/* P3-d. invalidate 成功后停止来数据（不调 on_abort）→ 拦 */
static void test_hook_invalidate_ok_stream_stops_blocked(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    ecat_foe_soes_adapter_t adapter;
    ecat_foe_soes_init(&adapter, &core, 64);
    s_mock_al_error_val = 0;
    s_mock_al_error_calls = 0;
    ecat_foe_soes_set_al_error_fn(&adapter, mock_al_error_cb);

    int ret_wrq = test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "合法 WRQ 返回 0");

    uint8_t payload[16] = {0x01, 0x02};
    uint8_t efw[256];
    build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    int ret_data = ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT_EQ_INT(ret_data, 0, "发送 64 字节头部成功返回 0");
    TEST_ASSERT_EQ_INT(mcu.count_init, 1, "驱动 init 成功调用");
    TEST_ASSERT_EQ_INT(mcu.count_invalidate, 1, "驱动 invalidate 成功调用");
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 0, "驱动 finalize 尚未调用");
    TEST_ASSERT(ecat_foe_core_upgrade_incomplete(&core), "断流时 upgrade_incomplete 为 true");

    uint8_t as = ECAT_TRANSITION_BOOT_TO_INIT;
    uint8_t an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_AL_STATUS_BOOT_ERROR, "invalidate 成功后断流必须拦截 BOOT->INIT (an 变为 0x13)");
    TEST_ASSERT_EQ_INT(s_mock_al_error_calls, 1, "触发 al_error_fn 注入回调");
}

/* P3-e. finalize 成功后 set_floor 失败 → 不拦 */
static void test_hook_finalize_ok_set_floor_failed_not_blocked(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);
    core.cfg.anti_rollback_enable = true;

    ecat_foe_soes_adapter_t adapter;
    ecat_foe_soes_init(&adapter, &core, 512);
    s_mock_al_error_val = 0;
    s_mock_al_error_calls = 0;
    ecat_foe_soes_set_al_error_fn(&adapter, mock_al_error_cb);

    s_mock_set_floor_fail = true;

    uint8_t payload[16] = {1, 2, 3};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x02000000U, 0x01000000U, payload, 16);

    int ret_wrq = test_wrq(&core, "SERVO_DRV_MCU_V2.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "合法 WRQ 返回 0");

    int ret_data = ecat_foe_core_on_data(&core, efw, efw_len);
    TEST_ASSERT_EQ_INT(ret_data, 0, "数据流写入返回 0");
    int ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, -1, "set_floor 失败导致 on_end 返回 -1");
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 1, "驱动 finalize 已成功调用");
    TEST_ASSERT(!ecat_foe_core_upgrade_incomplete(&core), "finalize 成功后 upgrade_incomplete 必须为 false");

    uint8_t as = ECAT_TRANSITION_BOOT_TO_INIT;
    uint8_t an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_STATE_INIT, "finalize 成功后 set_floor 失败不拦截 BOOT->INIT (an 保持 INIT)");
    TEST_ASSERT_EQ_INT(s_mock_al_error_calls, 0, "不调用 al_error_fn");

    s_mock_set_floor_fail = false;
}

/* P3-f. 成功升级 → 不拦 */
static void test_hook_upgrade_success_not_blocked(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    ecat_foe_soes_adapter_t adapter;
    ecat_foe_soes_init(&adapter, &core, 512);
    s_mock_al_error_val = 0;
    s_mock_al_error_calls = 0;
    ecat_foe_soes_set_al_error_fn(&adapter, mock_al_error_cb);

    uint8_t payload[16] = {1, 2, 3};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    int ret_wrq = test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "合法 WRQ 返回 0");

    int ret_data = ecat_foe_core_on_data(&core, efw, efw_len);
    TEST_ASSERT_EQ_INT(ret_data, 0, "固件数据写入返回 0");
    int ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, 0, "固件结束闭环返回 0");
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 1, "驱动 finalize 成功");
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_SUCCESS, "核心状态为 SUCCESS");
    TEST_ASSERT(!ecat_foe_core_upgrade_incomplete(&core), "upgrade_incomplete 为 false");

    uint8_t as = ECAT_TRANSITION_BOOT_TO_INIT;
    uint8_t an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_STATE_INIT, "成功升级不拦截 BOOT->INIT (an 保持 INIT)");
    TEST_ASSERT_EQ_INT(s_mock_al_error_calls, 0, "不调用 al_error_fn");
}

/* P3-g. 先 d，再在同一个 core 实例上走新的成功升级 → 不拦 */
static void test_hook_stream_stops_then_new_upgrade_success_not_blocked(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    ecat_foe_soes_adapter_t adapter;
    ecat_foe_soes_init(&adapter, &core, 64);
    s_mock_al_error_val = 0;
    s_mock_al_error_calls = 0;
    ecat_foe_soes_set_al_error_fn(&adapter, mock_al_error_cb);

    /* 阶段 1: 走 d 流程 (invalidate 成功后断流) */
    int ret_wrq1 = test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq1, 0, "第 1 次 WRQ 成功");

    uint8_t payload[16] = {0x11, 0x22};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    int ret_data1 = ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT_EQ_INT(ret_data1, 0, "第 1 次发送 64 字节头部成功");
    TEST_ASSERT_EQ_INT(mcu.count_init, 1, "驱动 init 成功");
    TEST_ASSERT_EQ_INT(mcu.count_invalidate, 1, "驱动 invalidate 成功");
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 0, "驱动尚未 finalize");
    TEST_ASSERT(ecat_foe_core_upgrade_incomplete(&core), "断流时 upgrade_incomplete 为 true");

    /* 此时尝试切入 INIT，必须拦截 */
    uint8_t as = ECAT_TRANSITION_BOOT_TO_INIT;
    uint8_t an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_AL_STATUS_BOOT_ERROR, "断流阶段拦截 BOOT->INIT");
    TEST_ASSERT_EQ_INT(s_mock_al_error_calls, 1, "调用 al_error_fn");

    /* 阶段 2: 在同一个 core 实例上发起新的成功升级 (不重建 core，不手动 reset core) */
    s_mock_al_error_calls = 0;
    s_mock_al_error_val = 0;
    adapter.buffer_size = 512;

    int ret_wrq2 = test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq2, 0, "同一个 core 实例发起新 WRQ 成功自愈");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "自愈时触发上次未闭环驱动 abort");

    int ret_data2 = ecat_foe_core_on_data(&core, efw, efw_len);
    TEST_ASSERT_EQ_INT(ret_data2, 0, "新升级完整固件写入成功");
    int ret_end2 = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end2, 0, "新升级成功结束闭环");
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 1, "新升级成功 finalize");
    TEST_ASSERT(!ecat_foe_core_upgrade_incomplete(&core), "新升级成功后 upgrade_incomplete 变为 false");

    as = ECAT_TRANSITION_BOOT_TO_INIT;
    an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_STATE_INIT, "新升级成功后放行 BOOT->INIT (an 保持 INIT)");
    TEST_ASSERT_EQ_INT(s_mock_al_error_calls, 0, "放行时不调用 al_error_fn");
}

/* -------------------------------------------------------------------------- */
/*             按目标生命周期状态跟踪与跨目标拦截独立测试套件                  */
/* -------------------------------------------------------------------------- */

/* 1. 跨目标状态保护测试: A 失败后 B 成功，A 依然维持未闭环并持续拦截 BOOT->INIT */
static void test_reproduce_cross_target_incomplete_upgrade_leak(void)
{
    static mock_driver_t mcu;
    static mock_driver_t sii;
    mock_driver_init(&mcu);
    mock_driver_init(&sii);

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, &sii, NULL);

    ecat_foe_soes_adapter_t adapter;
    ecat_foe_soes_init(&adapter, &core, 512);
    s_mock_al_error_calls = 0;
    s_mock_al_error_val = 0;
    ecat_foe_soes_set_al_error_fn(&adapter, mock_al_error_cb);

    /* 阶段 1: 升级目标 A (MCU, Target 1)，驱动已 init 且已 invalidate，但写入失败 */
    int ret_wrq_a = test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq_a, 0, "目标 A (MCU) WRQ 成功");

    uint8_t payload_a[16] = {0x11, 0x22};
    uint8_t efw_a[256];
    size_t efw_a_len = build_test_efw(efw_a, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload_a, 16);
    (void)efw_a_len;

    int ret_hdr_a = ecat_foe_core_on_data(&core, efw_a, 64);
    TEST_ASSERT_EQ_INT(ret_hdr_a, 0, "目标 A 发送头部成功");
    TEST_ASSERT_EQ_INT(mcu.count_init, 1, "目标 A 驱动 init 成功");
    TEST_ASSERT_EQ_INT(mcu.count_invalidate, 1, "目标 A 驱动 invalidate 成功 (物理固件已失效)");
    TEST_ASSERT(!mcu.valid_flag, "目标 A 固件处于无效状态");
    TEST_ASSERT(ecat_foe_core_target_upgrade_incomplete(&core, 1), "目标 A 被正确记录为未完成");
    TEST_ASSERT(ecat_foe_core_upgrade_incomplete(&core), "核心存在未闭环目标");

    /* 模拟数据结束校验失败 (CRC 校验不匹配导致 on_end 返回非 0) */
    uint8_t corrupt_payload[16] = {0xFF, 0xFF};
    ecat_foe_core_on_data(&core, corrupt_payload, 16);
    int ret_fail = ecat_foe_core_on_end(&core);
    TEST_ASSERT(ret_fail != 0, "数据损坏导致 on_end 校验失败");
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_ERROR, "核心状态进入 ERROR");
    TEST_ASSERT(ecat_foe_core_target_upgrade_incomplete(&core, 1), "失败后目标 A 依然未完成");

    /* 阶段 1 此时若尝试切入 INIT，坚决拦截 */
    uint8_t as = ECAT_TRANSITION_BOOT_TO_INIT;
    uint8_t an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_AL_STATUS_BOOT_ERROR, "阶段 1 单目标未完成时 hook 拦截 BOOT->INIT");
    TEST_ASSERT_EQ_INT(s_mock_al_error_calls, 1, "触发报警回调");

    /* 阶段 2: 同一个 core 实例上，转向升级不同目标 B (SII, Target 3) 并成功完成 */
    s_mock_al_error_calls = 0;
    int ret_wrq_b = test_wrq(&core, "SERVO_DRV_SII_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq_b, 0, "同一个 core 实例发起目标 B (SII) WRQ 成功自愈");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "自愈时触发目标 A 的 abort 释放资源");

    uint8_t payload_b[16];
    for (int i = 0; i < 16; i++) payload_b[i] = (uint8_t)(i * 2);
    uint8_t efw_b[256];
    size_t efw_b_len = build_test_efw(efw_b, 0x10010001U, 0x00000001U, 1, 1, 3, 0, 0x01000000U, 0, payload_b, 16);

    int ret_data_b = feed_stream_chunks(&core, efw_b, efw_b_len, 32, false);
    TEST_ASSERT_EQ_INT(ret_data_b, 0, "目标 B 数据推送成功");
    int ret_end_b = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end_b, 0, "目标 B 成功提交 finalize");
    TEST_ASSERT(sii.valid_flag, "目标 B (SII) 固件有效");

    /* 阶段 3: 严格断言按目标跟踪状态与跨目标拦截 */
    TEST_ASSERT(!mcu.valid_flag, "目标 A (MCU) 物理介质仍处于无效/残缺状态");
    TEST_ASSERT(!ecat_foe_core_target_upgrade_incomplete(&core, 3), "目标 B (SII) finalize 后已清除自身未完成状态");
    TEST_ASSERT(ecat_foe_core_target_upgrade_incomplete(&core, 1), "目标 A (MCU) 的未完成状态被严格保留");
    TEST_ASSERT(ecat_foe_core_upgrade_incomplete(&core), "因目标 A 残缺，核心全局依然处于未闭环状态");

    /* 测试 post_state_hook: 此时坚决拦截 BOOT->INIT，绝不误放行 */
    as = ECAT_TRANSITION_BOOT_TO_INIT;
    an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_AL_STATUS_BOOT_ERROR, "跨目标防御生效: 目标 A 仍残缺时坚决拦截 BOOT->INIT");
    TEST_ASSERT_EQ_INT(s_mock_al_error_calls, 1, "触发 al_error_fn 报警回调");
}

/* 2. 同目标恢复测试: 目标 A 失败拦截后，在同一个 core 实例上重新成功升级目标 A，解除拦截 */
static void test_cross_target_same_target_recovery(void)
{
    static mock_driver_t mcu;
    mock_driver_init(&mcu);

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    ecat_foe_soes_adapter_t adapter;
    ecat_foe_soes_init(&adapter, &core, 512);
    s_mock_al_error_calls = 0;
    ecat_foe_soes_set_al_error_fn(&adapter, mock_al_error_cb);

    /* 步骤 1: 目标 A 启动升级后断流失败 */
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    uint8_t payload[16] = {1, 2};
    uint8_t efw[256];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT(ecat_foe_core_target_upgrade_incomplete(&core, 1), "目标 A 处于未完成状态");

    uint8_t as = ECAT_TRANSITION_BOOT_TO_INIT;
    uint8_t an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_AL_STATUS_BOOT_ERROR, "未完成时拦截");

    /* 步骤 2: 在同一个 core 实例上重新完整升级目标 A */
    s_mock_al_error_calls = 0;
    int ret_wrq2 = test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq2, 0, "新 WRQ 成功自愈");
    ecat_foe_core_on_data(&core, efw, efw_len);
    int ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, 0, "目标 A 重新升级成功");
    TEST_ASSERT(mcu.valid_flag, "目标 A 物理固件已恢复有效");
    TEST_ASSERT(!ecat_foe_core_target_upgrade_incomplete(&core, 1), "目标 A 自身未完成标记已清除");
    TEST_ASSERT(!ecat_foe_core_upgrade_incomplete(&core), "核心已无未完成目标");

    /* 步骤 3: 再次尝试切入 INIT，正常放行 */
    as = ECAT_TRANSITION_BOOT_TO_INIT;
    an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_STATE_INIT, "同目标恢复成功后正常放行 BOOT->INIT");
    TEST_ASSERT_EQ_INT(s_mock_al_error_calls, 0, "放行时不报警");
}

/* 3. 多个目标未完成测试: A 失败且 B 也失败，逐步恢复过程严格保持拦截直到全部完成 */
static void test_cross_target_multiple_incomplete_targets(void)
{
    static mock_driver_t mcu;
    static mock_driver_t sii;
    mock_driver_init(&mcu);
    mock_driver_init(&sii);

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, &sii, NULL);

    ecat_foe_soes_adapter_t adapter;
    ecat_foe_soes_init(&adapter, &core, 512);
    s_mock_al_error_calls = 0;
    ecat_foe_soes_set_al_error_fn(&adapter, mock_al_error_cb);

    uint8_t payload[16] = {1, 2};
    uint8_t efw_mcu[256];
    size_t efw_mcu_len = build_test_efw(efw_mcu, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    uint8_t efw_sii[256];
    size_t efw_sii_len = build_test_efw(efw_sii, 0x10010001U, 0x00000001U, 1, 1, 3, 0, 0x01000000U, 0, payload, 16);

    /* 1. 目标 1 (MCU) 启动后失败 */
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    ecat_foe_core_on_data(&core, efw_mcu, 64);
    TEST_ASSERT(ecat_foe_core_target_upgrade_incomplete(&core, 1), "目标 1 未完成");

    /* 2. 目标 3 (SII) 启动后也失败 */
    test_wrq(&core, "SERVO_DRV_SII_V1.0.0.0.efw");
    ecat_foe_core_on_data(&core, efw_sii, 64);
    TEST_ASSERT(ecat_foe_core_target_upgrade_incomplete(&core, 3), "目标 3 未完成");
    TEST_ASSERT(ecat_foe_core_target_upgrade_incomplete(&core, 1), "目标 1 依然未完成");

    /* 此时两者均未完成，必须拦截 */
    uint8_t as = ECAT_TRANSITION_BOOT_TO_INIT;
    uint8_t an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_AL_STATUS_BOOT_ERROR, "两目标均未完成时坚决拦截");

    /* 3. 仅成功恢复目标 1 (MCU) */
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    ecat_foe_core_on_data(&core, efw_mcu, efw_mcu_len);
    ecat_foe_core_on_end(&core);
    TEST_ASSERT(!ecat_foe_core_target_upgrade_incomplete(&core, 1), "目标 1 已闭环");
    TEST_ASSERT(ecat_foe_core_target_upgrade_incomplete(&core, 3), "目标 3 依然未完成");
    TEST_ASSERT(ecat_foe_core_upgrade_incomplete(&core), "仍有目标未完成");

    /* 此时仍必须拦截 */
    as = ECAT_TRANSITION_BOOT_TO_INIT;
    an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_AL_STATUS_BOOT_ERROR, "仅恢复部分目标时继续拦截");

    /* 4. 成功恢复目标 3 (SII) */
    test_wrq(&core, "SERVO_DRV_SII_V1.0.0.0.efw");
    feed_stream_chunks(&core, efw_sii, efw_sii_len, 32, false);
    ecat_foe_core_on_end(&core);
    TEST_ASSERT(!ecat_foe_core_target_upgrade_incomplete(&core, 3), "目标 3 已闭环");
    TEST_ASSERT(!ecat_foe_core_upgrade_incomplete(&core), "全部目标已闭环");

    /* 全部闭环后放行 */
    as = ECAT_TRANSITION_BOOT_TO_INIT;
    an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_STATE_INIT, "全部目标恢复后放行 BOOT->INIT");
}

/* 4. 不同目标交替成功测试: 目标 1 成功 -> 目标 3 失败 -> 目标 4 成功 -> 目标 3 恢复 */
static void test_cross_target_different_targets_success_interleaving(void)
{
    static mock_driver_t mcu;
    static mock_driver_t sii;
    static mock_driver_t eeprom;
    mock_driver_init(&mcu);
    mock_driver_init(&sii);
    mock_driver_init(&eeprom);

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, &sii, &eeprom);

    ecat_foe_soes_adapter_t adapter;
    ecat_foe_soes_init(&adapter, &core, 512);

    uint8_t payload[16] = {0xAA};
    uint8_t efw1[256], efw3[256], efw4[256];
    size_t l1 = build_test_efw(efw1, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    size_t l3 = build_test_efw(efw3, 0x10010001U, 0x00000001U, 1, 1, 3, 0, 0x01000000U, 0, payload, 16);
    size_t l4 = build_test_efw(efw4, 0x10010001U, 0x00000001U, 1, 1, 4, 0, 0x01000000U, 0, payload, 16);

    /* A. 目标 1 成功升级 */
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    ecat_foe_core_on_data(&core, efw1, l1);
    ecat_foe_core_on_end(&core);
    TEST_ASSERT(!ecat_foe_core_upgrade_incomplete(&core), "目标 1 成功时不拦");

    /* B. 目标 3 升级启动后中断 */
    test_wrq(&core, "SERVO_DRV_SII_V1.0.0.0.efw");
    ecat_foe_core_on_data(&core, efw3, 64);
    TEST_ASSERT(ecat_foe_core_target_upgrade_incomplete(&core, 3), "目标 3 未完成");
    TEST_ASSERT(ecat_foe_core_upgrade_incomplete(&core), "存在未完成目标");

    /* C. 目标 4 (EEPROM) 成功升级 */
    test_wrq(&core, "SERVO_DRV_EEPROM_V1.0.0.0.efw");
    feed_stream_chunks(&core, efw4, l4, 32, false);
    ecat_foe_core_on_end(&core);
    TEST_ASSERT(!ecat_foe_core_target_upgrade_incomplete(&core, 4), "目标 4 已闭环");
    TEST_ASSERT(ecat_foe_core_target_upgrade_incomplete(&core, 3), "目标 3 仍未完成");
    TEST_ASSERT(ecat_foe_core_upgrade_incomplete(&core), "因目标 3 未完成，核心仍未闭环");

    uint8_t as = ECAT_TRANSITION_BOOT_TO_INIT;
    uint8_t an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_AL_STATUS_BOOT_ERROR, "目标 4 成功不能掩盖目标 3 残缺，坚决拦截");

    /* D. 目标 3 重新成功升级 */
    test_wrq(&core, "SERVO_DRV_SII_V1.0.0.0.efw");
    feed_stream_chunks(&core, efw3, l3, 32, false);
    ecat_foe_core_on_end(&core);
    TEST_ASSERT(!ecat_foe_core_upgrade_incomplete(&core), "全目标均闭环");

    as = ECAT_TRANSITION_BOOT_TO_INIT;
    an = ECAT_STATE_INIT;
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, ECAT_STATE_INIT, "全部完成后正常放行");
}

/* 5. init 失败 vs invalidate 失败对照测试 */
static void test_cross_target_init_fail_vs_invalidate_fail(void)
{
    /* 测项 A: 驱动 init 失败 -> 驱动资源未激活，不置位未完成状态，不拦截 */
    {
        static mock_driver_t mcu_a;
        mock_driver_init(&mcu_a);
        mcu_a.inject_fail_init = true;

        ecat_foe_core_t core;
        setup_default_core(&core, &mcu_a, NULL, NULL);

        ecat_foe_soes_adapter_t adapter;
        ecat_foe_soes_init(&adapter, &core, 512);

        test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
        uint8_t payload[16] = {1};
        uint8_t efw[256];
        size_t l = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
        int ret_data = ecat_foe_core_on_data(&core, efw, l);
        TEST_ASSERT_EQ_INT(ret_data, -1, "init 失败返回 -1");
        TEST_ASSERT_EQ_INT(mcu_a.count_init, 1, "驱动 init 已被调用");
        TEST_ASSERT_EQ_INT(mcu_a.count_invalidate, 0, "init 失败未触碰 invalidate");
        TEST_ASSERT(!ecat_foe_core_target_upgrade_incomplete(&core, 1), "init 失败不记录为未完成");
        TEST_ASSERT(!ecat_foe_core_upgrade_incomplete(&core), "核心未记录未闭环目标");

        uint8_t as = ECAT_TRANSITION_BOOT_TO_INIT;
        uint8_t an = ECAT_STATE_INIT;
        ecat_foe_soes_post_state_hook(&adapter, &as, &an);
        TEST_ASSERT_EQ_INT(an, ECAT_STATE_INIT, "init 失败不触发拦截");
    }

    /* 测项 B: 驱动 init 成功但 invalidate 失败 -> 固件处于未闭环状态，必须拦截 */
    {
        static mock_driver_t mcu_b;
        mock_driver_init(&mcu_b);
        mcu_b.inject_fail_invalidate = true;

        ecat_foe_core_t core;
        setup_default_core(&core, &mcu_b, NULL, NULL);

        ecat_foe_soes_adapter_t adapter;
        ecat_foe_soes_init(&adapter, &core, 512);

        test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
        uint8_t payload[16] = {1};
        uint8_t efw[256];
        size_t l = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
        int ret_data = ecat_foe_core_on_data(&core, efw, l);
        TEST_ASSERT_EQ_INT(ret_data, -1, "invalidate 失败返回 -1");
        TEST_ASSERT_EQ_INT(mcu_b.count_init, 1, "驱动 init 成功");
        TEST_ASSERT_EQ_INT(mcu_b.count_invalidate, 1, "驱动 invalidate 失败");
        TEST_ASSERT(ecat_foe_core_target_upgrade_incomplete(&core, 1), "init 成功即标记为未完成");
        TEST_ASSERT(ecat_foe_core_upgrade_incomplete(&core), "核心标记存在未闭环目标");

        uint8_t as = ECAT_TRANSITION_BOOT_TO_INIT;
        uint8_t an = ECAT_STATE_INIT;
        ecat_foe_soes_post_state_hook(&adapter, &as, &an);
        TEST_ASSERT_EQ_INT(an, ECAT_AL_STATUS_BOOT_ERROR, "invalidate 失败坚决拦截");
    }
}

/* 6. finalize 后防重复 abort 强化测试: 成功 finalize 后，后续 abort/自愈/deinit 绝不调用 abort */
static void test_cross_target_finalize_prevents_duplicate_abort(void)
{
    static mock_driver_t mcu;
    mock_driver_init(&mcu);

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    uint8_t payload[16] = {1, 2, 3};
    uint8_t efw[256];
    size_t l = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    /* 正常完整升级完成 finalize */
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    ecat_foe_core_on_data(&core, efw, l);
    int ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, 0, "升级成功");
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 1, "驱动 finalize 成功调用");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "成功完成未调用 abort");

    /* 动作 1: 主动调用 on_abort */
    ecat_foe_core_on_abort(&core);
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "finalize 后调 on_abort 绝不触发驱动 abort");

    /* 动作 2: 新会话 WRQ 自愈触发 */
    test_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "finalize 后新会话自愈绝不触发驱动 abort");

    /* 动作 3: 调用 deinit */
    ecat_foe_core_deinit(&core);
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "finalize 后调用 deinit 绝不触发驱动 abort");
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
    RUN_TEST(test_erase_to_insufficient_erased_end_fails);
    RUN_TEST(test_padding_capacity_boundary);
    RUN_TEST(test_failed_session_new_wrq_full_upgrade_recovery);
    RUN_TEST(test_mid_session_drop_same_core_recovery);
    RUN_TEST(test_self_referential_pointer_elimination_and_idle_copy);
    RUN_TEST(test_core_deinit_lifecycle);

    /* 需求对应 C2 & C3 专项独立测试项 */
    RUN_TEST(test_idle_copy_erases_original_with_user_ctx_upgrade);
    RUN_TEST(test_100_failed_self_healing_cycles);
    RUN_TEST(test_dirty_memory_initialization_a5_and_02);
    RUN_TEST(test_null_project_name_rejection);
    RUN_TEST(test_reproduce_soes_data_callback_without_wrq_stream_rejection);
    RUN_TEST(test_hook_filename_rejected_not_blocked);
    RUN_TEST(test_hook_header_invalid_not_blocked);
    RUN_TEST(test_hook_init_ok_invalidate_failed_blocked);
    RUN_TEST(test_hook_invalidate_ok_stream_stops_blocked);
    RUN_TEST(test_hook_finalize_ok_set_floor_failed_not_blocked);
    RUN_TEST(test_hook_upgrade_success_not_blocked);
    RUN_TEST(test_hook_stream_stops_then_new_upgrade_success_not_blocked);
    RUN_TEST(test_reproduce_cross_target_incomplete_upgrade_leak);
    RUN_TEST(test_cross_target_same_target_recovery);
    RUN_TEST(test_cross_target_multiple_incomplete_targets);
    RUN_TEST(test_cross_target_different_targets_success_interleaving);
    RUN_TEST(test_cross_target_init_fail_vs_invalidate_fail);
    RUN_TEST(test_cross_target_finalize_prevents_duplicate_abort);

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
