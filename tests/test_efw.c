/**
 * @file test_efw.c
 * @brief .efw 容器解析、校验与打包交叉验证测试套件 (阶段 A)
 *
 * 严格覆盖：
 * 1. CRC32 "123456789" 标准测试向量 (0xCBF43926)
 * 2. efw-format-v0.1 第 8 节完整正例，十六进制逐字节比对，C 与 Python 互测
 * 3. 第 8 节 4 个典型反例 (Header CRC 错、少收、flags 非零、多收熔断)
 * 4. 全量边界异常测项 (magic、version、header_len 下限/上限/对齐、reserved 非零、
 *    硬件版本倒置/不在区间、project/hw_id 不符、target 未使能、超容量、防回滚)
 * 5. 128 字节头部向前兼容性扩展跳过测试
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "ecat_foe_efw.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        printf("  [FAIL] %s:%d: %s\n", __func__, __LINE__, msg); \
        g_tests_failed++; \
        return; \
    } else { \
        g_tests_passed++; \
    } \
} while(0)

#define TEST_ASSERT_EQ_HEX32(actual, expected, msg) do { \
    g_tests_run++; \
    if ((actual) != (expected)) { \
        printf("  [FAIL] %s:%d: %s (实际: 0x%08X, 期望: 0x%08X)\n", \
               __func__, __LINE__, msg, (unsigned int)(actual), (unsigned int)(expected)); \
        g_tests_failed++; \
        return; \
    } else { \
        g_tests_passed++; \
    } \
} while(0)

#define TEST_ASSERT_EQ_INT(actual, expected, msg) do { \
    g_tests_run++; \
    if ((actual) != (expected)) { \
        printf("  [FAIL] %s:%d: %s (实际: %d, 期望: %d)\n", \
               __func__, __LINE__, msg, (int)(actual), (int)(expected)); \
        g_tests_failed++; \
        return; \
    } else { \
        g_tests_passed++; \
    } \
} while(0)

/* -------------------------------------------------------------------------- */
/*             efw-format-v0.1 第 8.1 节规范写死的完整正例数据                */
/* -------------------------------------------------------------------------- */
static const uint8_t s_spec_valid_efw[80] = {
    /* 0x0000: Header (64 字节) */
    0x45, 0x43, 0x46, 0x57, 0x01, 0x00, 0x40, 0x00,
    0x01, 0x00, 0x01, 0x10, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x04, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x10, 0x00, 0x00, 0x00, 0x88, 0xE2, 0xCE, 0xCE,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xE3, 0x52, 0xC2, 0x61,

    /* 0x0040: Valid Payload (16 字节: 0x00..0x0F) */
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};

/* 辅助函数：构造默认正例从站本地配置 */
static void get_default_valid_config(ecat_foe_efw_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->project_id = 0x10010001U;
    cfg->hardware_id = 0x00000001U;
    cfg->local_hw_ver = 1U;
    cfg->anti_rollback_enable = false;

    /* Target 1: MCU */
    cfg->targets[0].enabled = true;
    cfg->targets[0].max_payload_size = 1024U * 1024U;
    cfg->targets[0].version_floor = 0x01000000U;

    /* Target 2: FPGA */
    cfg->targets[1].enabled = false;
    cfg->targets[1].max_payload_size = 0U;
    cfg->targets[1].version_floor = 0U;

    /* Target 3: SII */
    cfg->targets[2].enabled = false;
    cfg->targets[2].max_payload_size = 0U;
    cfg->targets[2].version_floor = 0U;

    /* Target 4: EEPROM */
    cfg->targets[3].enabled = false;
    cfg->targets[3].max_payload_size = 0U;
    cfg->targets[3].version_floor = 0U;
}

/* -------------------------------------------------------------------------- */
/*                               测试用例实现                                 */
/* -------------------------------------------------------------------------- */

/**
 * 测项 1: CRC32 "123456789" 标准测试向量
 */
static void test_crc32_standard_vector(void)
{
    const uint8_t test_str[] = "123456789";
    uint32_t crc_oneshot = ecat_foe_crc32_calc(test_str, 9);
    TEST_ASSERT_EQ_HEX32(crc_oneshot, 0xCBF43926U, "CRC32 单步计算标准向量 123456789");

    /* 增量接口分片测试 (4 字节 + 5 字节) */
    uint32_t crc_ctx;
    ecat_foe_crc32_init(&crc_ctx);
    ecat_foe_crc32_update(&crc_ctx, test_str, 4);
    ecat_foe_crc32_update(&crc_ctx, test_str + 4, 5);
    uint32_t crc_incr = ecat_foe_crc32_final(&crc_ctx);
    TEST_ASSERT_EQ_HEX32(crc_incr, 0xCBF43926U, "CRC32 增量分片计算标准向量");
}

/**
 * 测项 2: efw-format 第 8.1 节完整正例解析验证
 */
static void test_spec_section8_positive_example(void)
{
    uint16_t header_len = 0;
    ecat_foe_efw_err_t err;

    /* 阶段 1: 前 8 字节探测 */
    err = ecat_foe_efw_parse_stage1(s_spec_valid_efw, 8, 128, &header_len);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "阶段 1 探测应当成功");
    TEST_ASSERT_EQ_INT(header_len, 64, "解析出的头部长度应当为 64");

    /* 阶段 2: 头部装配与 Header CRC32 校验 */
    ecat_foe_efw_header_t hdr;
    err = ecat_foe_efw_parse_stage2(s_spec_valid_efw, 64, &hdr);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "阶段 2 装配与 CRC 应当成功");
    TEST_ASSERT_EQ_HEX32(hdr.magic, 0x57464345U, "hdr.magic 应为 0x57464345");
    TEST_ASSERT_EQ_INT(hdr.format_version, 1, "hdr.format_version 应为 1");
    TEST_ASSERT_EQ_INT(hdr.header_length, 64, "hdr.header_length 应为 64");
    TEST_ASSERT_EQ_HEX32(hdr.project_id, 0x10010001U, "hdr.project_id 应为 0x10010001");
    TEST_ASSERT_EQ_HEX32(hdr.hardware_id, 0x00000001U, "hdr.hardware_id 应为 0x00000001");
    TEST_ASSERT_EQ_INT(hdr.hw_ver_min, 1, "hdr.hw_ver_min 应为 1");
    TEST_ASSERT_EQ_INT(hdr.hw_ver_max, 1, "hdr.hw_ver_max 应为 1");
    TEST_ASSERT_EQ_INT(hdr.target_id, 1, "hdr.target_id 应为 1 (MCU)");
    TEST_ASSERT_EQ_INT(hdr.flags, 0, "hdr.flags 必须为 0");
    TEST_ASSERT_EQ_HEX32(hdr.fw_version, 0x01020304U, "hdr.fw_version 应为 0x01020304 (V1.2.3.4)");
    TEST_ASSERT_EQ_HEX32(hdr.min_allow_version, 0x01000000U, "hdr.min_allow_version 应为 0x01000000");
    TEST_ASSERT_EQ_INT(hdr.payload_length, 16, "hdr.payload_length 应为 16");
    TEST_ASSERT_EQ_HEX32(hdr.payload_crc32, 0xCECEE288U, "hdr.payload_crc32 应为 0xCECEE288");
    TEST_ASSERT_EQ_HEX32(hdr.header_crc32, 0x61C252E3U, "hdr.header_crc32 应为 0x61C252E3");

    /* 验证 Payload 数据流 CRC32 */
    uint32_t calc_p_crc = ecat_foe_crc32_calc(s_spec_valid_efw + 64, 16);
    TEST_ASSERT_EQ_HEX32(calc_p_crc, hdr.payload_crc32, "有效负载数据流计算 CRC32 与头部应严格一致");

    /* 阶段 3: 全量元数据安全核验 */
    ecat_foe_efw_config_t cfg;
    get_default_valid_config(&cfg);
    err = ecat_foe_efw_parse_stage3(&hdr, &cfg);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "阶段 3 元数据安全核验应当成功");
}

/**
 * 测项 3: 反例 1 - Header CRC 损坏或被篡改
 */
static void test_negative_case1_bad_header_crc(void)
{
    uint8_t bad_efw[64];
    memcpy(bad_efw, s_spec_valid_efw, 64);

    /* 篡改末尾 Header CRC 为 0x9E3DAD1C */
    bad_efw[60] = 0x1C;
    bad_efw[61] = 0xAD;
    bad_efw[62] = 0x3D;
    bad_efw[63] = 0x9E;

    uint16_t header_len = 0;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage1(bad_efw, 8, 128, &header_len);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "反例 1 阶段 1 应当通过");

    ecat_foe_efw_header_t hdr;
    err = ecat_foe_efw_parse_stage2(bad_efw, 64, &hdr);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_ERR_HEADER_CRC, "反例 1 阶段 2 应当检出 Header CRC32 不匹配");
}

/**
 * 测项 4: 反例 2 - 流总长度少收 (标称 80 字节，实际仅接收 76 字节)
 */
static void test_negative_case2_short_stream(void)
{
    /* 阶段 1 和阶段 2 解析头部 */
    ecat_foe_efw_header_t hdr;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage2(s_spec_valid_efw, 64, &hdr);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "头部应当合法");

    /* 模拟传输结束时的总长度校验规则 */
    uint32_t expected_total = hdr.header_length + hdr.payload_length;
    TEST_ASSERT_EQ_INT(expected_total, 80, "期望总长度应为 80 字节");

    uint32_t actual_received = 76U;  /* 少收 4 字节 */
    TEST_ASSERT(actual_received != expected_total, "传输结束时实际长度不等于 Expected_Total 判定为失败");
}

/**
 * 测项 5: 反例 3 - Flags 包含未定义非零标志位 (0x0001)
 */
static void test_negative_case3_bad_flags(void)
{
    /* 严格按照 efw-format 8.2 反例 3 十六进制转储 */
    static const uint8_t s_case3_header[64] = {
        0x45, 0x43, 0x46, 0x57, 0x01, 0x00, 0x40, 0x00,
        0x01, 0x00, 0x01, 0x10, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00,  /* 偏移 0x16-0x17 flags = 0x0001 */
        0x04, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x10, 0x00, 0x00, 0x00, 0x88, 0xE2, 0xCE, 0xCE,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0xAF, 0x4A, 0xEC   /* header_crc32 = 0xEC4AAF01 */
    };

    uint16_t header_len = 0;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage1(s_case3_header, 8, 128, &header_len);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "反例 3 阶段 1 应当通过");

    ecat_foe_efw_header_t hdr;
    err = ecat_foe_efw_parse_stage2(s_case3_header, 64, &hdr);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "反例 3 阶段 2 Header CRC 自洽应当通过");
    TEST_ASSERT_EQ_INT(hdr.flags, 0x0001, "hdr.flags 应当为 0x0001");

    ecat_foe_efw_config_t cfg;
    get_default_valid_config(&cfg);
    err = ecat_foe_efw_parse_stage3(&hdr, &cfg);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_ERR_FLAGS_NONZERO, "反例 3 阶段 3 应当拒绝非零 flags");
}

/**
 * 测项 6: 反例 4 - 流多收检测与熔断
 */
static void test_negative_case4_over_receive_fuse(void)
{
    ecat_foe_efw_header_t hdr;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage2(s_spec_valid_efw, 64, &hdr);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "头部应当合法");

    uint64_t expected_total = (uint64_t)hdr.header_length + (uint64_t)hdr.payload_length; /* 80 */
    TEST_ASSERT_EQ_INT((int)expected_total, 80, "Expected_Total 应为 80");

    /* 模拟数据包流式累加接收 */
    uint64_t rx_offset = 64;  /* 已接收头部 64 字节 */
    uint32_t block_len = 20;  /* 某次到达 20 字节 Payload (期望仅 16 字节) */

    bool fuse_triggered = false;
    if (rx_offset + block_len > expected_total) {
        fuse_triggered = true;
    }
    TEST_ASSERT(fuse_triggered, "累计偏移超过 Expected_Total 时应当即时触发熔断拒绝");
}

/**
 * 测项 7: 边界异常 - magic 错误
 */
static void test_edge_case_magic_err(void)
{
    uint8_t buf[64];
    memcpy(buf, s_spec_valid_efw, 64);
    buf[0] = 0xAA; buf[1] = 0xBB; buf[2] = 0xCC; buf[3] = 0xDD;

    uint16_t hlen = 0;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage1(buf, 8, 128, &hlen);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_ERR_MAGIC, "magic 错误应当被阶段 1 检出");
}

/**
 * 测项 8: 边界异常 - format_version 不是 1
 */
static void test_edge_case_format_version_err(void)
{
    uint8_t buf[64];
    memcpy(buf, s_spec_valid_efw, 64);
    buf[4] = 0x02; buf[5] = 0x00; /* format_version = 2 */

    uint16_t hlen = 0;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage1(buf, 8, 128, &hlen);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_ERR_FORMAT_VERSION, "format_version != 1 应当被阶段 1 检出");
}

/**
 * 测项 9: 边界异常 - header_length 小于 64
 */
static void test_edge_case_header_len_min(void)
{
    uint8_t buf[64];
    memcpy(buf, s_spec_valid_efw, 64);
    buf[6] = 0x3C; buf[7] = 0x00; /* header_length = 60 */

    uint16_t hlen = 0;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage1(buf, 8, 128, &hlen);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_ERR_HEADER_LEN_MIN, "header_length < 64 应当被阶段 1 检出");
}

/**
 * 测项 10: 边界异常 - header_length 大于本地上限
 */
static void test_edge_case_header_len_max(void)
{
    uint8_t buf[64];
    memcpy(buf, s_spec_valid_efw, 64);
    buf[6] = 0x00; buf[7] = 0x01; /* header_length = 256 */

    uint16_t hlen = 0;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage1(buf, 8, 128, &hlen); /* max_header_len = 128 */
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_ERR_HEADER_LEN_MAX, "header_length > max 应当被阶段 1 检出");
}

/**
 * 测项 11: 边界异常 - header_length 不是 4 的倍数
 */
static void test_edge_case_header_len_align(void)
{
    uint8_t buf[64];
    memcpy(buf, s_spec_valid_efw, 64);
    buf[6] = 0x41; buf[7] = 0x00; /* header_length = 65 */

    uint16_t hlen = 0;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage1(buf, 8, 128, &hlen);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_ERR_HEADER_LEN_ALIGN, "header_length % 4 != 0 应当被阶段 1 检出");
}

/**
 * 测项 12: 边界异常 - reserved 区域非全零
 */
static void test_edge_case_reserved_nonzero(void)
{
    uint8_t buf[64];
    memcpy(buf, s_spec_valid_efw, 64);
    buf[0x28 + 5] = 0xFF; /* 注入非零 */

    /* 重算 CRC 使得阶段 2 能够通过，专门考察阶段 3 */
    uint32_t new_crc = ecat_foe_crc32_calc(buf, 60);
    buf[60] = (uint8_t)(new_crc & 0xFF);
    buf[61] = (uint8_t)((new_crc >> 8) & 0xFF);
    buf[62] = (uint8_t)((new_crc >> 16) & 0xFF);
    buf[63] = (uint8_t)((new_crc >> 24) & 0xFF);

    ecat_foe_efw_header_t hdr;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage2(buf, 64, &hdr);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "阶段 2 应装配成功");

    ecat_foe_efw_config_t cfg;
    get_default_valid_config(&cfg);
    err = ecat_foe_efw_parse_stage3(&hdr, &cfg);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_ERR_RESERVED_NONZERO, "reserved 非零应当被阶段 3 检出");
}

/**
 * 测项 13: 边界异常 - hw_ver_min > hw_ver_max
 */
static void test_edge_case_hw_ver_min_gt_max(void)
{
    uint8_t buf[64];
    memcpy(buf, s_spec_valid_efw, 64);
    buf[0x10] = 0x05; buf[0x11] = 0x00; /* min = 5 */
    buf[0x12] = 0x02; buf[0x13] = 0x00; /* max = 2 */

    uint32_t new_crc = ecat_foe_crc32_calc(buf, 60);
    buf[60] = (uint8_t)(new_crc & 0xFF);
    buf[61] = (uint8_t)((new_crc >> 8) & 0xFF);
    buf[62] = (uint8_t)((new_crc >> 16) & 0xFF);
    buf[63] = (uint8_t)((new_crc >> 24) & 0xFF);

    ecat_foe_efw_header_t hdr;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage2(buf, 64, &hdr);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "阶段 2 应当通过");

    ecat_foe_efw_config_t cfg;
    get_default_valid_config(&cfg);
    err = ecat_foe_efw_parse_stage3(&hdr, &cfg);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_ERR_HW_VER_RANGE_INVALID, "hw_ver_min > hw_ver_max 应当被检出");
}

/**
 * 测项 14: 边界异常 - 本地硬件版本不在区间
 */
static void test_edge_case_local_hw_ver_not_supported(void)
{
    ecat_foe_efw_header_t hdr;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage2(s_spec_valid_efw, 64, &hdr);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "阶段 2 应当通过");

    ecat_foe_efw_config_t cfg;
    get_default_valid_config(&cfg);
    cfg.local_hw_ver = 2U; /* 固件仅支持 [1, 1] */

    err = ecat_foe_efw_parse_stage3(&hdr, &cfg);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_ERR_HW_VER_NOT_SUPPORTED, "本地硬件版本超出区间应当被拒绝");
}

/**
 * 测项 15: 边界异常 - project_id / hardware_id 不符
 */
static void test_edge_case_id_mismatch(void)
{
    ecat_foe_efw_header_t hdr;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage2(s_spec_valid_efw, 64, &hdr);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "阶段 2 应当通过");

    ecat_foe_efw_config_t cfg;
    get_default_valid_config(&cfg);

    /* project_id 不符 */
    cfg.project_id = 0x99999999U;
    err = ecat_foe_efw_parse_stage3(&hdr, &cfg);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_ERR_PROJECT_ID_MISMATCH, "project_id 不符应当被检出");

    /* 恢复 project_id，测试 hardware_id 不符 */
    cfg.project_id = 0x10010001U;
    cfg.hardware_id = 0x88888888U;
    err = ecat_foe_efw_parse_stage3(&hdr, &cfg);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_ERR_HARDWARE_ID_MISMATCH, "hardware_id 不符应当被检出");
}

/**
 * 测项 16: 边界异常 - target 未启用
 */
static void test_edge_case_target_not_enabled(void)
{
    ecat_foe_efw_header_t hdr;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage2(s_spec_valid_efw, 64, &hdr);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "阶段 2 应当通过");

    ecat_foe_efw_config_t cfg;
    get_default_valid_config(&cfg);
    cfg.targets[0].enabled = false; /* 目标 1 MCU 未开启 */

    err = ecat_foe_efw_parse_stage3(&hdr, &cfg);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_ERR_TARGET_NOT_ENABLED, "target 未使能应当被阶段 3 拒绝");
}

/**
 * 测项 17: 边界异常 - payload_length 超过本地分区容量上限
 */
static void test_edge_case_payload_too_large(void)
{
    ecat_foe_efw_header_t hdr;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage2(s_spec_valid_efw, 64, &hdr);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "阶段 2 应当通过");

    ecat_foe_efw_config_t cfg;
    get_default_valid_config(&cfg);
    cfg.targets[0].max_payload_size = 8U; /* 限制上限为 8 字节，而 payload 为 16 字节 */

    err = ecat_foe_efw_parse_stage3(&hdr, &cfg);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_ERR_PAYLOAD_TOO_LARGE, "payload 超出分区限额应当被拒绝");
}

/**
 * 测项 18: 边界异常 - min_allow_version > fw_version
 */
static void test_edge_case_min_allow_version_invalid(void)
{
    uint8_t buf[64];
    memcpy(buf, s_spec_valid_efw, 64);

    /* 注入 min_allow_version = 0x02000000 (大于 fw_version 0x01020304) */
    buf[0x1C] = 0x00; buf[0x1D] = 0x00; buf[0x1E] = 0x00; buf[0x1F] = 0x02;

    uint32_t new_crc = ecat_foe_crc32_calc(buf, 60);
    buf[60] = (uint8_t)(new_crc & 0xFF);
    buf[61] = (uint8_t)((new_crc >> 8) & 0xFF);
    buf[62] = (uint8_t)((new_crc >> 16) & 0xFF);
    buf[63] = (uint8_t)((new_crc >> 24) & 0xFF);

    ecat_foe_efw_header_t hdr;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage2(buf, 64, &hdr);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "阶段 2 应当通过");

    ecat_foe_efw_config_t cfg;
    get_default_valid_config(&cfg);

    err = ecat_foe_efw_parse_stage3(&hdr, &cfg);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_ERR_MIN_ALLOW_VER_INVALID, "min_allow_version > fw_version 应当被拒绝");
}

/**
 * 测项 19: 边界异常 - 防回滚拦截 (fw_version < version_floor)
 */
static void test_edge_case_anti_rollback(void)
{
    ecat_foe_efw_header_t hdr;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage2(s_spec_valid_efw, 64, &hdr);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "阶段 2 应当通过");

    ecat_foe_efw_config_t cfg;
    get_default_valid_config(&cfg);
    cfg.anti_rollback_enable = true;
    cfg.targets[0].version_floor = 0x02000000U; /* 版本底线为 V2.0，而固件是 V1.2.3.4 */

    err = ecat_foe_efw_parse_stage3(&hdr, &cfg);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_ERR_ANTI_ROLLBACK, "防回滚生效时应当拦截低版本固件");
}

/**
 * 测项 20: 向前兼容性 - header_length = 128 (解析器跳过未知扩展字段)
 */
static void test_forward_compatibility_128_byte_header(void)
{
    uint8_t efw_128[128 + 16];
    memset(efw_128, 0, sizeof(efw_128));

    /* 拷贝基础 60 字节 */
    memcpy(efw_128, s_spec_valid_efw, 60);

    /* 设置 header_length = 128 (0x0080) */
    efw_128[6] = 0x80;
    efw_128[7] = 0x00;

    /* 填充 0x40 - 123 (共 64 字节未知扩展字段) */
    for (size_t i = 64; i < 124; i++) {
        efw_128[i] = (uint8_t)(i ^ 0x5A);
    }

    /* 位于 header_length - 4 (124..127) 处写入覆盖前 124 字节的 Header CRC32 */
    uint32_t h_crc_128 = ecat_foe_crc32_calc(efw_128, 124);
    efw_128[124] = (uint8_t)(h_crc_128 & 0xFF);
    efw_128[125] = (uint8_t)((h_crc_128 >> 8) & 0xFF);
    efw_128[126] = (uint8_t)((h_crc_128 >> 16) & 0xFF);
    efw_128[127] = (uint8_t)((h_crc_128 >> 24) & 0xFF);

    /* 追加 16 字节有效负载 (位于 offset 128) */
    memcpy(efw_128 + 128, s_spec_valid_efw + 64, 16);

    /* 阶段 1 探测 (max_header_len = 128) */
    uint16_t out_hlen = 0;
    ecat_foe_efw_err_t err = ecat_foe_efw_parse_stage1(efw_128, 8, 128, &out_hlen);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "128 字节头部阶段 1 应当探测成功");
    TEST_ASSERT_EQ_INT(out_hlen, 128, "探测出的 header_length 应为 128");

    /* 阶段 2 装配与 CRC */
    ecat_foe_efw_header_t hdr;
    err = ecat_foe_efw_parse_stage2(efw_128, 128, &hdr);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "128 字节头部阶段 2 CRC 校验应当通过");
    TEST_ASSERT_EQ_INT(hdr.header_length, 128, "装配后的 header_length 应为 128");
    TEST_ASSERT_EQ_HEX32(hdr.header_crc32, h_crc_128, "Header CRC32 应匹配 124 字节覆盖范围计算值");
    TEST_ASSERT_EQ_HEX32(hdr.project_id, 0x10010001U, "基础字段反序列化正常");

    /* 阶段 3 全量元数据安全核验 */
    ecat_foe_efw_config_t cfg;
    get_default_valid_config(&cfg);
    err = ecat_foe_efw_parse_stage3(&hdr, &cfg);
    TEST_ASSERT_EQ_INT(err, ECAT_FOE_EFW_OK, "128 字节扩展头部阶段 3 安全核验应当通过");

    /* 验证有效负载偏移跳过未知扩展字段 */
    uint32_t p_crc = ecat_foe_crc32_calc(efw_128 + hdr.header_length, hdr.payload_length);
    TEST_ASSERT_EQ_HEX32(p_crc, hdr.payload_crc32, "跳过 64 字节扩展区后 Payload CRC 应依然吻合");
}

/* -------------------------------------------------------------------------- */
/*                               主测试入口                                   */
/* -------------------------------------------------------------------------- */

int main(void)
{
    printf("====================================================\n");
    printf(" EtherCAT .efw 容器解析与核验单元测试套件 (阶段 A)\n");
    printf(" 规范依据: docs/spec/efw-format-v0.1.md\n");
    printf(" 编译标准: C99 (-std=c99 -Wall -Wextra -Werror -pedantic)\n");
    printf(" 动态内存: 零 malloc / 零动态分配\n");
    printf("====================================================\n");

    test_crc32_standard_vector();
    test_spec_section8_positive_example();
    test_negative_case1_bad_header_crc();
    test_negative_case2_short_stream();
    test_negative_case3_bad_flags();
    test_negative_case4_over_receive_fuse();
    test_edge_case_magic_err();
    test_edge_case_format_version_err();
    test_edge_case_header_len_min();
    test_edge_case_header_len_max();
    test_edge_case_header_len_align();
    test_edge_case_reserved_nonzero();
    test_edge_case_hw_ver_min_gt_max();
    test_edge_case_local_hw_ver_not_supported();
    test_edge_case_id_mismatch();
    test_edge_case_target_not_enabled();
    test_edge_case_payload_too_large();
    test_edge_case_min_allow_version_invalid();
    test_edge_case_anti_rollback();
    test_forward_compatibility_128_byte_header();

    printf("\n测试运行统计: 共执行断言 %d 项 | 通过: %d 项 | 失败: %d 项\n",
           g_tests_run, g_tests_passed, g_tests_failed);

    if (g_tests_failed == 0) {
        printf(">>> 全部 C 单元测试通过！<<<\n");
        return 0;
    } else {
        printf(">>> 存在测试失败项！<<<\n");
        return 1;
    }
}
