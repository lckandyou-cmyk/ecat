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

#define TEST_ASSERT(cond, msg) do { \
    g_core_tests_run++; \
    if (!(cond)) { \
        printf("  [FAIL] %s:%d: %s\n", __func__, __LINE__, msg); \
        g_core_tests_failed++; \
        return; \
    } else { \
        g_core_tests_passed++; \
    } \
} while(0)

#define TEST_ASSERT_EQ_INT(actual, expected, msg) do { \
    g_core_tests_run++; \
    if ((actual) != (expected)) { \
        printf("  [FAIL] %s:%d: %s (实际: %d, 期望: %d)\n", \
               __func__, __LINE__, msg, (int)(actual), (int)(expected)); \
        g_core_tests_failed++; \
        return; \
    } else { \
        g_core_tests_passed++; \
    } \
} while(0)

/* -------------------------------------------------------------------------- */
/*                            平台防回滚桩接口实现                            */
/* -------------------------------------------------------------------------- */
static uint32_t s_mock_version_floors[ECAT_FOE_TARGET_COUNT] = {0};

int port_get_version_floor(uint16_t target_id, uint32_t *floor_ver)
{
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

static void setup_default_core(ecat_foe_core_t *core, mock_driver_t *mock_mcu, mock_driver_t *mock_sii, mock_driver_t *mock_eeprom)
{
    ecat_foe_core_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.project_id = 0x10010001U;
    cfg.project_name = "SERVO_DRV";
    cfg.hardware_id = 0x00000001U;
    cfg.local_hw_ver = 1U;
    cfg.anti_rollback_enable = false;

    /* Target 1: MCU */
    cfg.targets[0].target_id = 1;
    cfg.targets[0].enabled = true;
    cfg.targets[0].max_payload_size = 384U * 1024U;
    cfg.targets[0].write_unit = 8U;
    cfg.targets[0].driver = (mock_mcu != NULL) ? &mock_mcu->driver : NULL;
    cfg.targets[0].user_ctx = mock_mcu;

    /* Target 2: FPGA (禁用) */
    cfg.targets[1].target_id = 2;
    cfg.targets[1].enabled = false;

    /* Target 3: SII */
    cfg.targets[2].target_id = 3;
    cfg.targets[2].enabled = true;
    cfg.targets[2].max_payload_size = 4U * 1024U;
    cfg.targets[2].write_unit = 2U;
    cfg.targets[2].driver = (mock_sii != NULL) ? &mock_sii->driver : NULL;
    cfg.targets[2].user_ctx = mock_sii;

    /* Target 4: EEPROM */
    cfg.targets[3].target_id = 4;
    cfg.targets[3].enabled = true;
    cfg.targets[3].max_payload_size = 8U * 1024U;
    cfg.targets[3].write_unit = 4U;
    cfg.targets[3].driver = (mock_eeprom != NULL) ? &mock_eeprom->driver : NULL;
    cfg.targets[3].user_ctx = mock_eeprom;

    ecat_foe_core_init(core, &cfg);
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
/*                               阶段 B 单元测试用例                           */
/* -------------------------------------------------------------------------- */

/**
 * 测项 1: 正常升级流程 (MCU、SII 偶数、EEPROM) 与调用顺序验证
 */
static void test_normal_upgrade_and_call_order(void)
{
    mock_driver_t mcu, sii, eeprom;
    mock_driver_init(&mcu);
    mock_driver_init(&sii);
    mock_driver_init(&eeprom);

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, &sii, &eeprom);

    /* 1. MCU 固件正常升级 */
    uint8_t payload[32];
    for (int i = 0; i < 32; i++) payload[i] = (uint8_t)(i + 1);

    uint8_t efw_buf[512];
    size_t efw_len = build_test_efw(efw_buf, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01020304U, 0x01000000U, payload, 32);

    int ret_wrq = ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.2.3.4.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "WRQ 应当成功");

    /* 模拟 64 字节块推送 */
    int ret_data = feed_stream_chunks(&core, efw_buf, efw_len, 64, false);
    TEST_ASSERT_EQ_INT(ret_data, 0, "数据块推送应当成功");
    int ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, 0, "结束提交应当成功");
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_SUCCESS, "核心状态应处于 SUCCESS");
    TEST_ASSERT(mcu.valid_flag, "升级成功后 valid_flag 应置为 true");

    /* 顺序断言: init -> invalidate -> erase_to -> write -> read -> finalize */
    TEST_ASSERT(mcu.op_count >= 6, "至少记录 6 步驱动操作");
    TEST_ASSERT_EQ_INT(mcu.op_history[0], MOCK_OP_INIT, "第 1 步必须为 INIT");
    TEST_ASSERT_EQ_INT(mcu.op_history[1], MOCK_OP_INVALIDATE, "第 2 步必须为 INVALIDATE");
    TEST_ASSERT_EQ_INT(mcu.op_history[2], MOCK_OP_ERASE_TO, "第 3 步必须为 ERASE_TO");
    TEST_ASSERT_EQ_INT(mcu.op_history[3], MOCK_OP_WRITE, "第 4 步必须为 WRITE");
    TEST_ASSERT_EQ_INT(mcu.op_history[mcu.op_count - 2], MOCK_OP_READ, "倒数第 2 步必须为 READ");
    TEST_ASSERT_EQ_INT(mcu.op_history[mcu.op_count - 1], MOCK_OP_FINALIZE, "最后 1 步必须为 FINALIZE");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 0, "成功时 abort 绝对不可被调用 (互斥)");

    /* 2. SII 固件 (偶数长度 16 字节) */
    mock_driver_reset(&sii);
    setup_default_core(&core, &mcu, &sii, &eeprom);
    efw_len = build_test_efw(efw_buf, 0x10010001U, 0x00000001U, 1, 1, 3, 0, 0x01000000U, 0, payload, 16);
    ret_wrq = ecat_foe_core_on_wrq(&core, "SERVO_DRV_SII_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "SII WRQ 应当成功");
    ret_data = feed_stream_chunks(&core, efw_buf, efw_len, 32, false);
    TEST_ASSERT_EQ_INT(ret_data, 0, "SII 推送应当成功");
    ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, 0, "SII 结束应当成功");
    TEST_ASSERT(sii.valid_flag, "SII valid_flag 应当为 true");

    /* 3. EEPROM 固件 (48 字节) */
    mock_driver_reset(&eeprom);
    setup_default_core(&core, &mcu, &sii, &eeprom);
    efw_len = build_test_efw(efw_buf, 0x10010001U, 0x00000001U, 1, 1, 4, 0, 0x01000100U, 0, payload, 16);
    ret_wrq = ecat_foe_core_on_wrq(&core, "SERVO_DRV_EEPROM_V1.0.1.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "EEPROM WRQ 应当成功");
    ret_data = feed_stream_chunks(&core, efw_buf, efw_len, 16, false);
    TEST_ASSERT_EQ_INT(ret_data, 0, "EEPROM 数据推送应当成功");
    ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, 0, "EEPROM 结束应当成功");
    TEST_ASSERT(eeprom.valid_flag, "EEPROM valid_flag 应当为 true");
}

/**
 * 测项 2: 小 buffer_size (16, 32) 头跨多次回调与 0 长度结束包
 */
static void test_small_buffers_and_zero_len_termination(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    uint8_t payload[64];
    for (int i = 0; i < 64; i++) payload[i] = (uint8_t)(i ^ 0xA5);

    uint8_t efw[512];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01020304U, 0, payload, 64);

    int ret_wrq = ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.2.3.4.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "WRQ 应当成功");

    /* 以 16 字节极小切块模拟 SOES 分包 (头部 64 字节跨 4 次回调才集齐) */
    int ret = feed_stream_chunks(&core, efw, efw_len, 16, true); /* 末尾追加 0 长度结束包 */
    TEST_ASSERT_EQ_INT(ret, 0, "跨块推送及 0 长度结束应当成功闭环");
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_SUCCESS, "最终状态应为 SUCCESS");
    TEST_ASSERT(mcu.valid_flag, "驱动提交成功");
}

/**
 * 测项 3: 头阶段各类拒绝场景驱动调用次数必须为 0，文件名拒绝驱动调用次数为 0
 */
static void test_driver_zero_calls_on_rejections(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    /* A. 文件名非法拒绝 */
    int ret_wrq = ecat_foe_core_on_wrq(&core, "BAD_FILENAME.bin");
    TEST_ASSERT(ret_wrq != 0, "非法文件名应当拒绝");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "文件名拒绝驱动 init 必须为 0");
    TEST_ASSERT_EQ_INT(mcu.count_invalidate, 0, "文件名拒绝驱动 invalidate 必须为 0");
    TEST_ASSERT_EQ_INT(mcu.count_erase, 0, "文件名拒绝驱动 erase 必须为 0");
    TEST_ASSERT_EQ_INT(mcu.count_write, 0, "文件名拒绝驱动 write 必须为 0");

    /* B. 项目名不匹配拒绝 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    ret_wrq = ecat_foe_core_on_wrq(&core, "OTHER_PROJ_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, ECAT_FOE_ERRCODE_ACCESS, "项目名不符应回 0x8002");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "项目名不符驱动调用必须为 0");

    /* C. 目标未启用拒绝 (FPGA) */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    ret_wrq = ecat_foe_core_on_wrq(&core, "SERVO_DRV_FPGA_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, ECAT_FOE_ERRCODE_NOTFOUND, "目标禁用应回 0x8001");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "目标禁用驱动调用必须为 0");

    /* D. 头部 Header CRC 校验失败 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    ret_wrq = ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.2.3.4.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "WRQ 应当成功");

    uint8_t efw[128];
    build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01020304U, 0, NULL, 0);
    efw[63] ^= 0xFF; /* 篡改 Header CRC */
    int ret_data = ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT(ret_data != 0, "Header CRC 错误必须被拒绝");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "Header CRC 错误驱动 init 必须为 0");
    TEST_ASSERT_EQ_INT(mcu.count_invalidate, 0, "Header CRC 错误驱动 invalidate 必须为 0");
    TEST_ASSERT_EQ_INT(mcu.count_erase, 0, "Header CRC 错误驱动 erase 必须为 0");
    TEST_ASSERT_EQ_INT(mcu.count_write, 0, "Header CRC 错误驱动 write 必须为 0");

    /* E. 双向交叉一致性比对失败 (头部版本与文件名版本不一致) */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    ret_wrq = ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.2.3.4.efw"); /* 声明 V1.2.3.4 */
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "WRQ 应当成功");
    build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x02000000U, 0, NULL, 0); /* 头部装入 V2.0.0.0 */
    ret_data = ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT(ret_data != 0, "版本交叉不一致应当拒绝");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "交叉不一致驱动 init 必须为 0");
    TEST_ASSERT_EQ_INT(mcu.count_invalidate, 0, "交叉不一致驱动 invalidate 必须为 0");
}

/**
 * 测项 4: 传输异常熔断 (少收、多收整块拒写、流 CRC 错、回读坏、驱动各接口故障)
 */
static void test_failure_scenarios_and_mutex(void)
{
    mock_driver_t mcu;
    ecat_foe_core_t core;

    uint8_t payload[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t efw[128];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    /* 1. 少收 (Under-receive) */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    ecat_foe_core_on_data(&core, efw, 64); /* 仅推送头部，不发 payload */
    int ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT(ret_end != 0, "少收应当失败");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_UNDER_RECEIVE, "应当报 UNDER_RECEIVE");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "少收应当调 abort 释放");
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 0, "少收 finalize 必须为 0");

    /* 2. 多收熔断 (Over-receive: 整块拒绝且不写入) */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    ecat_foe_core_on_data(&core, efw, 64 + 8); /* 送 8 字节有效数据 */
    uint32_t writes_before = mcu.count_write;

    uint8_t bad_chunk[20];
    memset(bad_chunk, 0x55, sizeof(bad_chunk)); /* 剩余只有 8 字节，这里送 20 字节必然超长 */
    int ret_over = ecat_foe_core_on_data(&core, bad_chunk, sizeof(bad_chunk));
    TEST_ASSERT(ret_over != 0, "超长块必须立即熔断拒绝");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_OVER_RECEIVE, "应当报 OVER_RECEIVE");
    TEST_ASSERT_EQ_INT(mcu.count_write, writes_before, "超长块绝对不可向存储执行写入");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "多收应当调用 abort");

    /* 3. 流 CRC 错 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    uint8_t corrupted_efw[128];
    memcpy(corrupted_efw, efw, efw_len);
    corrupted_efw[64] ^= 0xFF; /* 篡改 Payload 内容使得与头中 payload_crc32 不符 */
    feed_stream_chunks(&core, corrupted_efw, efw_len, 64, false);
    ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT(ret_end != 0, "流 CRC 错必须拒绝提交");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_STREAM_CRC_MISMATCH, "应当报 STREAM_CRC_MISMATCH");
    TEST_ASSERT_EQ_INT(mcu.count_finalize, 0, "CRC 错 finalize 必须为 0");

    /* 4. 物理介质底层回读损坏 (Readback CRC 错) */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    mcu.inject_corrupt_read = true; /* 注入回读损坏 */
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    feed_stream_chunks(&core, efw, efw_len, 64, false);
    ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT(ret_end != 0, "回读损坏必须拒绝提交");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_READBACK_CRC_MISMATCH, "应当报 READBACK_CRC_MISMATCH");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "回读坏调用 abort");

    /* 5. 驱动 erase 失败 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    mcu.inject_fail_erase = true;
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    int ret_erase = feed_stream_chunks(&core, efw, efw_len, 64, false);
    TEST_ASSERT(ret_erase != 0, "erase 失败必须中断");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_DRV_ERASE, "报 DRV_ERASE");

    /* 6. 驱动 write 失败 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    mcu.inject_fail_write = true;
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    int ret_write = feed_stream_chunks(&core, efw, efw_len, 64, false);
    TEST_ASSERT(ret_write != 0, "write 失败必须中断");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_DRV_WRITE, "报 DRV_WRITE");

    /* 7. 驱动 finalize 失败 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    mcu.inject_fail_finalize = true;
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    feed_stream_chunks(&core, efw, efw_len, 64, false);
    ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT(ret_end != 0, "finalize 失败应当报错");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "finalize 失败必须转调 abort");
}

/**
 * 测项 5: 会话自愈与中途 on_abort
 */
static void test_session_self_healing_and_abort(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    uint8_t payload[16] = {1, 2, 3, 4};
    uint8_t efw[128];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    (void)efw_len;

    /* A. 中途调用 on_abort */
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_DATA, "应当已进入 DATA 态");
    ecat_foe_core_on_abort(&core);
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_IDLE, "on_abort 后应当重置为 IDLE");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "应当调用驱动 abort 一次");

    /* B. 模拟协议栈直接断开、未调 abort，直接来新 WRQ 会话自愈 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    ecat_foe_core_on_data(&core, efw, 64); /* 进入写状态 */

    /* 此时主站重新下发新 WRQ */
    int ret_heal = ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_heal, 0, "自愈后新 WRQ 应当成功");
    TEST_ASSERT_EQ_INT(mcu.count_abort, 1, "上一残留会话的驱动 abort 必须被自愈触发调用一次");
}

/**
 * 测项 6: 负载大小恰好等于分区容量与超 1 字节
 */
static void test_partition_capacity_boundary(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    /* 设定本地容量限制为 64 字节 */
    core.cfg.targets[0].max_payload_size = 64U;

    uint8_t payload[65];
    memset(payload, 0x11, sizeof(payload));

    /* 1. 恰好等于 64 字节 -> 允许通过 */
    uint8_t efw[256];
    size_t len_ok = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 64);
    (void)len_ok;
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    int ret_ok = ecat_foe_core_on_data(&core, efw, 64); /* 推送头 */
    TEST_ASSERT_EQ_INT(ret_ok, 0, "容量恰好相等应当核验通过");

    /* 2. 超出 1 字节 (65 字节) -> 必须被阶段 3 拒绝 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    core.cfg.targets[0].max_payload_size = 64U;

    size_t len_bad = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 65);
    (void)len_bad;
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    int ret_bad = ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT(ret_bad != 0, "超 1 字节必须被拒绝");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_HDR_VERIFY, "报 HDR_VERIFY");
    TEST_ASSERT_EQ_INT(mcu.count_init, 0, "容量超限驱动 init 绝不能调");
}

/**
 * 测项 7: write_unit 为 8 和 256 时的错位残留写入、回读数据一致性与 padding 不计入 CRC
 */
static void test_misaligned_residual_writes(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    /* A. write_unit = 8, payload = 13 字节 (差 3 字节不对齐，末尾以 3 个 0xFF 补齐写入) */
    core.cfg.targets[0].write_unit = 8U;
    uint8_t payload13[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};

    uint8_t efw[128];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload13, 13);
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    feed_stream_chunks(&core, efw, efw_len, 16, false);
    int ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, 0, "残留补齐写入应当成功");
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_SUCCESS, "升级成功");

    /* 检查 Flash 物理内容: 前 13 字节必须原样一致，后 3 字节补齐为 0xFF */
    TEST_ASSERT(memcmp(mcu.mem, payload13, 13) == 0, "前 13 字节物理内容必须与原负载完全一致");
    TEST_ASSERT_EQ_INT(mcu.mem[13], 0xFF, "第 14 字节补齐 0xFF");
    TEST_ASSERT_EQ_INT(mcu.mem[14], 0xFF, "第 15 字节补齐 0xFF");
    TEST_ASSERT_EQ_INT(mcu.mem[15], 0xFF, "第 16 字节补齐 0xFF");

    /* B. write_unit = 256, payload = 300 字节 (跨两个 256 扇区，末尾补 212 个 0xFF) */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    core.cfg.targets[0].write_unit = 256U;

    uint8_t payload300[300];
    for (int i = 0; i < 300; i++) payload300[i] = (uint8_t)(i + 0x20);

    uint8_t efw_large[512];
    efw_len = build_test_efw(efw_large, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload300, 300);
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V1.0.0.0.efw");
    feed_stream_chunks(&core, efw_large, efw_len, 64, false);
    ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, 0, "256 字节对齐残留写入成功");
    TEST_ASSERT(memcmp(mcu.mem, payload300, 300) == 0, "前 300 字节必须完全吻合");
}

/**
 * 测项 8: 防回滚更新与拦截验证
 */
static void test_anti_rollback_behavior(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);
    core.cfg.anti_rollback_enable = true;

    /* 初始版本下限为 V1.0.0.0 (0x01000000) */
    port_set_version_floor(1, 0x01000000U);

    uint8_t payload[16] = {0};
    uint8_t efw[128];

    /* 1. 尝试降级至 V0.9.0.0 (0x00090000) -> 必须被拦截 */
    size_t len_down = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x00090000U, 0, payload, 16);
    (void)len_down;
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V0.9.0.0.efw");
    int ret_down = ecat_foe_core_on_data(&core, efw, 64);
    TEST_ASSERT(ret_down != 0, "降级版本必须被防回滚拦截");
    TEST_ASSERT_EQ_INT(core.last_error, ECAT_FOE_CORE_ERR_HDR_VERIFY, "防回滚报错");

    /* 2. 正常升级到 V2.0.0.0，声明 min_allow_version = V1.5.0.0 (0x01050000) */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    core.cfg.anti_rollback_enable = true;

    size_t len_up = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x02000000U, 0x01050000U, payload, 16);
    ecat_foe_core_on_wrq(&core, "SERVO_DRV_MCU_V2.0.0.0.efw");
    feed_stream_chunks(&core, efw, len_up, 64, false);
    int ret_end = ecat_foe_core_on_end(&core);
    TEST_ASSERT_EQ_INT(ret_end, 0, "升级应当成功");

    /* 验证版本下限是否更新为 max(0x01000000, 0x01050000) = 0x01050000 */
    uint32_t updated_floor = 0;
    port_get_version_floor(1, &updated_floor);
    TEST_ASSERT_EQ_INT(updated_floor, 0x01050000U, "版本下限应成功持久化更新为 0x01050000");
}

/**
 * 测项 9: SOES 适配层闭环与 BOOT->INIT 迁移拦截
 */
static void test_soes_adapter_integration(void)
{
    mock_driver_t mcu;
    mock_driver_init(&mcu);

    ecat_foe_core_t core;
    setup_default_core(&core, &mcu, NULL, NULL);

    ecat_foe_soes_adapter_t adapter;
    ecat_foe_soes_init(&adapter, &core, 512); /* SOES 缓冲 512 字节 */

    foe_file_cfg_t file_cfg;
    memset(&file_cfg, 0, sizeof(file_cfg));

    uint8_t payload[16] = {1, 2, 3};
    uint8_t efw[128];
    size_t efw_len = build_test_efw(efw, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    /* 1. 正常通过 SOES 适配层回调写入 */
    int ret_wrq = ecat_foe_soes_on_wrq(&adapter, "SERVO_DRV_MCU_V1.0.0.0.efw");
    TEST_ASSERT_EQ_INT(ret_wrq, 0, "SOES on_wrq 应当成功");

    /* 本次发送总共 efw_len 字节 (80 字节)，由于 80 < buffer_size(512)，自动触发结束闭环 */
    uint32_t ret_cb = ecat_foe_soes_write_cb(&file_cfg, efw, efw_len);
    TEST_ASSERT_EQ_INT(ret_cb, 0, "SOES 写入回调应成功闭环并返回 0");
    TEST_ASSERT_EQ_INT(core.state, ECAT_FOE_STATE_SUCCESS, "核心状态成功");
    TEST_ASSERT(!adapter.last_commit_failed, "last_commit_failed 必须为 false");

    /* 2. 失败场景触发 post_state_hook 拦截 */
    mock_driver_reset(&mcu);
    setup_default_core(&core, &mcu, NULL, NULL);
    ecat_foe_soes_init(&adapter, &core, 512);

    ecat_foe_soes_on_wrq(&adapter, "SERVO_DRV_MCU_V1.0.0.0.efw");
    efw[63] ^= 0xFF; /* 破坏 Header CRC 触发失败 */
    ret_cb = ecat_foe_soes_write_cb(&file_cfg, efw, efw_len);
    TEST_ASSERT(ret_cb != 0, "失败回调必须返回非零");
    TEST_ASSERT(adapter.last_commit_failed, "last_commit_failed 必须置为 true");

    /* 模拟主站请求 BOOT -> INIT (0x13) */
    uint8_t as = 0x13U;
    uint8_t an = 0x01U; /* 初始 INIT */
    ecat_foe_soes_post_state_hook(&adapter, &as, &an);
    TEST_ASSERT_EQ_INT(an, 0x13U, "post hook 必须改写为 BOOT + Error (0x13)");
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

    test_normal_upgrade_and_call_order();
    test_small_buffers_and_zero_len_termination();
    test_driver_zero_calls_on_rejections();
    test_failure_scenarios_and_mutex();
    test_session_self_healing_and_abort();
    test_partition_capacity_boundary();
    test_misaligned_residual_writes();
    test_anti_rollback_behavior();
    test_soes_adapter_integration();

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
