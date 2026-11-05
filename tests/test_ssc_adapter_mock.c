/**
 * @file test_ssc_adapter_mock.c
 * @brief Beckhoff SSC 适配层抽象契约测试 (Mock 验证环境)
 *
 * 重要边界声明：
 * 本测试为基于抽象契约的单元测试桩 (Mock/Stub)，用于验证 ecat_foe_ssc 适配层
 * 对通用核心机事件生命周期的转接与错误码映射。
 * 当前本机未安装 Beckhoff 官方 SSC 源码包，未在真实 SSC 协议栈与硬件环境下联调。
 * 绝不能将本 Mock 测试称作真实 SSC 验证！
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "ecat_foe_ssc.h"
#include "ecat_foe_efw.h"
#include "mock_driver.h"

#define TEST_PROJECT_ID      0x10010001U
#define TEST_HARDWARE_ID     0x00000001U
#define TEST_HW_VER          1U
#define TEST_FW_VER          0x01000000U /* 1.0.0.0 */
#define TEST_PROJECT_NAME    "SERVO_DRV"
#define TEST_MAX_SIZE        (256U * 1024U)

static mock_driver_t s_mock_drv;

/* 外部防回滚桩函数实现 (满足链接需求) */
int port_get_version_floor(uint16_t target_id, uint32_t *floor_ver)
{
    (void)target_id;
    if (floor_ver != NULL) {
        *floor_ver = 0;
    }
    return 0;
}

int port_set_version_floor(uint16_t target_id, uint32_t new_floor_ver)
{
    (void)target_id;
    (void)new_floor_ver;
    return 0;
}

static void setup_core_and_adapter(ecat_foe_core_t *core, ecat_foe_ssc_adapter_t *adapter, uint32_t packet_size)
{
    mock_driver_init(&s_mock_drv);

    ecat_foe_core_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.project_id = TEST_PROJECT_ID;
    cfg.project_name = TEST_PROJECT_NAME;
    cfg.hardware_id = TEST_HARDWARE_ID;
    cfg.local_hw_ver = TEST_HW_VER;
    cfg.anti_rollback_enable = false;
    cfg.max_header_size = 128U;

    cfg.targets[0].target_id = 1;
    cfg.targets[0].enabled = true;
    cfg.targets[0].max_payload_size = TEST_MAX_SIZE;
    cfg.targets[0].write_unit = 8U;
    cfg.targets[0].driver = &s_mock_drv.driver;
    cfg.targets[0].user_ctx = &s_mock_drv;

    assert(ecat_foe_core_init(core, &cfg) == 0);
    ecat_foe_ssc_init(adapter, core, packet_size);
}

/* 构造合法的固件数据包 (64 字节头部 + 负载) */
static size_t build_valid_firmware(uint8_t *out_buf, size_t buf_size, const uint8_t *payload, uint32_t payload_len)
{
    assert(buf_size >= 64U + payload_len);
    memset(out_buf, 0, 64);
    out_buf[0] = 0x45; out_buf[1] = 0x43; out_buf[2] = 0x46; out_buf[3] = 0x57; /* ECFW */
    out_buf[4] = 0x01; out_buf[5] = 0x00;                                         /* v1 */
    out_buf[6] = 0x40; out_buf[7] = 0x00;                                         /* 64 字节头 */

    out_buf[8]  = (uint8_t)(TEST_PROJECT_ID & 0xFF);
    out_buf[9]  = (uint8_t)((TEST_PROJECT_ID >> 8) & 0xFF);
    out_buf[10] = (uint8_t)((TEST_PROJECT_ID >> 16) & 0xFF);
    out_buf[11] = (uint8_t)((TEST_PROJECT_ID >> 24) & 0xFF);

    out_buf[12] = (uint8_t)(TEST_HARDWARE_ID & 0xFF);
    out_buf[13] = (uint8_t)((TEST_HARDWARE_ID >> 8) & 0xFF);
    out_buf[14] = (uint8_t)((TEST_HARDWARE_ID >> 16) & 0xFF);
    out_buf[15] = (uint8_t)((TEST_HARDWARE_ID >> 24) & 0xFF);

    out_buf[16] = (uint8_t)(TEST_HW_VER & 0xFF);
    out_buf[17] = (uint8_t)((TEST_HW_VER >> 8) & 0xFF);
    out_buf[18] = (uint8_t)(TEST_HW_VER & 0xFF);
    out_buf[19] = (uint8_t)((TEST_HW_VER >> 8) & 0xFF);

    out_buf[20] = 0x01; out_buf[21] = 0x00;                                       /* target_id = 1 */
    out_buf[22] = 0x00; out_buf[23] = 0x00;                                       /* flags = 0 */

    out_buf[24] = (uint8_t)(TEST_FW_VER & 0xFF);
    out_buf[25] = (uint8_t)((TEST_FW_VER >> 8) & 0xFF);
    out_buf[26] = (uint8_t)((TEST_FW_VER >> 16) & 0xFF);
    out_buf[27] = (uint8_t)((TEST_FW_VER >> 24) & 0xFF);

    out_buf[32] = (uint8_t)(payload_len & 0xFF);
    out_buf[33] = (uint8_t)((payload_len >> 8) & 0xFF);
    out_buf[34] = (uint8_t)((payload_len >> 16) & 0xFF);
    out_buf[35] = (uint8_t)((payload_len >> 24) & 0xFF);

    uint32_t p_crc = (payload != NULL && payload_len > 0) ? ecat_foe_crc32_calc(payload, payload_len) : 0U;
    out_buf[36] = (uint8_t)(p_crc & 0xFF);
    out_buf[37] = (uint8_t)((p_crc >> 8) & 0xFF);
    out_buf[38] = (uint8_t)((p_crc >> 16) & 0xFF);
    out_buf[39] = (uint8_t)((p_crc >> 24) & 0xFF);

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

int main(void)
{
    printf("=== 运行 Beckhoff SSC 适配层抽象契约测试 (Mock 环境) ===\n");

    ecat_foe_core_t core;
    ecat_foe_ssc_adapter_t adapter;
    uint8_t fw_buf[512];
    uint8_t payload[64];
    memset(payload, 0x5A, sizeof(payload));

    /* [TEST 1] 标准全流程验证: WRQ -> DATA -> 短包 EOF */
    {
        printf("\n[TEST 1] 验证 SSC 适配层标准升级全流程 (短包定界 EOF)...");
        setup_core_and_adapter(&core, &adapter, 64U);
        uint8_t short_payload[32];
        memset(short_payload, 0x5A, sizeof(short_payload));
        build_valid_firmware(fw_buf, sizeof(fw_buf), short_payload, (uint32_t)sizeof(short_payload));

        /* 1. WRQ 阶段 */
        const char *valid_fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
        uint16_t err = ecat_foe_ssc_on_write(&adapter, valid_fn, (uint16_t)strlen(valid_fn), 0);
        assert(err == 0);

        /* 2. DATA 阶段 (分块下发: 64 字节头部块，等于 max_packet_size) */
        err = ecat_foe_ssc_on_data(&adapter, fw_buf, 64U);
        assert(err == 0);

        /* 3. DATA 阶段 (32 字节负载块，因 32 < 64 触发短包结束) */
        err = ecat_foe_ssc_on_data(&adapter, fw_buf + 64U, (uint16_t)sizeof(short_payload));
        assert(err == 0);

        /* 验证驱动落盘成功，有效标记已写入 */
        assert(s_mock_drv.valid_flag == true);
        printf(" [PASS]\n");
    }

    /* [TEST 2] WRQ 阶段项目名不匹配拦截 */
    {
        printf("[TEST 2] 验证 SSC WRQ 阶段项目名称不匹配拦截 (返回 0x8001)...");
        setup_core_and_adapter(&core, &adapter, 128U);

        const char *invalid_fn = "OTHER_DRV_MCU_V1.0.0.0.efw";
        uint16_t err = ecat_foe_ssc_on_write(&adapter, invalid_fn, (uint16_t)strlen(invalid_fn), 0);
        assert(err == ECAT_FOE_SSC_ERR_NOTFOUND); /* 0x8001 */
        assert(!adapter.session_active);
        printf(" [PASS]\n");
    }

    /* [TEST 3] 未建立会话直接写入数据拦截 */
    {
        printf("[TEST 3] 验证未建立会话直接写数据被安全拦截 (返回 0x8004)...");
        setup_core_and_adapter(&core, &adapter, 128U);

        uint16_t err = ecat_foe_ssc_on_data(&adapter, fw_buf, 64U);
        assert(err == ECAT_FOE_SSC_ERR_ILLEGAL); /* 0x8004 */
        printf(" [PASS]\n");
    }

    /* [TEST 4] 独立 0 长度包定界验证 */
    {
        printf("[TEST 4] 验证独立 0 长度数据包显式触发 EOF 定界...");
        setup_core_and_adapter(&core, &adapter, 64U);
        build_valid_firmware(fw_buf, sizeof(fw_buf), payload, (uint32_t)sizeof(payload));

        const char *valid_fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
        assert(ecat_foe_ssc_on_write(&adapter, valid_fn, (uint16_t)strlen(valid_fn), 0) == 0);

        /* 灌入完整数据块 (每块 64 字节，恰好等于 max_packet_size，不走短包分支) */
        assert(ecat_foe_ssc_on_data(&adapter, fw_buf, 64U) == 0);
        assert(ecat_foe_ssc_on_data(&adapter, fw_buf + 64U, 64U) == 0);
        assert(s_mock_drv.valid_flag == false); /* 未发 0 长度包前尚未 finalize */

        /* 发送独立 0 长度数据包 */
        assert(ecat_foe_ssc_on_data(&adapter, NULL, 0U) == 0);
        assert(s_mock_drv.valid_flag == true); /* 0 长度包触发成功 finalize */
        printf(" [PASS]\n");
    }

    /* [TEST 5] 底层 Flash 驱动校验失败时错误码映射 */
    {
        printf("[TEST 5] 验证底层驱动写入失败时映射为 0x800B 并中止...");
        setup_core_and_adapter(&core, &adapter, 64U);
        build_valid_firmware(fw_buf, sizeof(fw_buf), payload, (uint32_t)sizeof(payload));

        const char *valid_fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
        assert(ecat_foe_ssc_on_write(&adapter, valid_fn, (uint16_t)strlen(valid_fn), 0) == 0);
        assert(ecat_foe_ssc_on_data(&adapter, fw_buf, 64U) == 0);

        /* 注入驱动写入故障 */
        s_mock_drv.inject_fail_write = true;
        uint16_t err = ecat_foe_ssc_on_data(&adapter, fw_buf + 64U, 64U);
        assert(err == ECAT_FOE_SSC_ERR_PROGERROR); /* 0x800B */
        assert(!adapter.session_active);
        assert(s_mock_drv.valid_flag == false);
        printf(" [PASS]\n");
    }

    /* [TEST 6] 异常中止与新 WRQ 会话自愈恢复 */
    {
        printf("[TEST 6] 验证中途 abort 后在同一 adapter 上平滑重启新会话...");
        setup_core_and_adapter(&core, &adapter, 128U);

        const char *valid_fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
        assert(ecat_foe_ssc_on_write(&adapter, valid_fn, (uint16_t)strlen(valid_fn), 0) == 0);
        assert(adapter.session_active);

        /* 模拟主站发 abort 中止 */
        ecat_foe_ssc_on_abort(&adapter, 0x800BU);
        assert(!adapter.session_active);

        /* 重新接收新 WRQ，验证平滑自愈恢复 */
        assert(ecat_foe_ssc_on_write(&adapter, valid_fn, (uint16_t)strlen(valid_fn), 0) == 0);
        assert(adapter.session_active);
        printf(" [PASS]\n");
    }

    /* [TEST 7] 单包超出 max_packet_size 上限防御拦截 */
    {
        printf("[TEST 7] 验证单包超出 max_packet_size 上限拦截 (返回 0x8004 并中止)...");
        setup_core_and_adapter(&core, &adapter, 64U);
        build_valid_firmware(fw_buf, sizeof(fw_buf), payload, (uint32_t)sizeof(payload));

        const char *valid_fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
        assert(ecat_foe_ssc_on_write(&adapter, valid_fn, (uint16_t)strlen(valid_fn), 0) == 0);

        /* 注入超限包: 长度 96 字节 > max_packet_size 64 字节 */
        uint16_t err = ecat_foe_ssc_on_data(&adapter, fw_buf, 96U);
        assert(err == ECAT_FOE_SSC_ERR_ILLEGAL); /* 0x8004 */
        assert(!adapter.session_active);
        printf(" [PASS]\n");
    }

    /* [TEST 8] 短包定界成功落盘后，主站补发 0 长度包幂等确认 */
    {
        printf("[TEST 8] 验证短包定界落盘后补发 0 长度包幂等成功 (返回 0)...");
        setup_core_and_adapter(&core, &adapter, 64U);
        uint8_t short_payload[32];
        memset(short_payload, 0x5A, sizeof(short_payload));
        build_valid_firmware(fw_buf, sizeof(fw_buf), short_payload, (uint32_t)sizeof(short_payload));

        const char *valid_fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
        assert(ecat_foe_ssc_on_write(&adapter, valid_fn, (uint16_t)strlen(valid_fn), 0) == 0);

        /* 分块下发: 64 满包 + 32 短包 (触发成功落盘) */
        assert(ecat_foe_ssc_on_data(&adapter, fw_buf, 64U) == 0);
        assert(ecat_foe_ssc_on_data(&adapter, fw_buf + 64U, 32U) == 0);
        assert(s_mock_drv.valid_flag == true);
        assert(core.state == ECAT_FOE_STATE_SUCCESS);

        /* 主站补发 0 长度握手确认包，验证幂等通过返回 0 */
        assert(ecat_foe_ssc_on_data(&adapter, NULL, 0U) == 0);
        assert(core.state == ECAT_FOE_STATE_SUCCESS);
        printf(" [PASS]\n");
    }

    /* [TEST 9] 升级成功后调用 on_abort 保护终态不被重置为 IDLE */
    {
        printf("[TEST 9] 验证升级成功终态在调用 on_abort 时保持 SUCCESS 标记...");
        setup_core_and_adapter(&core, &adapter, 64U);
        uint8_t short_payload[32];
        memset(short_payload, 0x5A, sizeof(short_payload));
        build_valid_firmware(fw_buf, sizeof(fw_buf), short_payload, (uint32_t)sizeof(short_payload));

        const char *valid_fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
        assert(ecat_foe_ssc_on_write(&adapter, valid_fn, (uint16_t)strlen(valid_fn), 0) == 0);
        assert(ecat_foe_ssc_on_data(&adapter, fw_buf, 64U) == 0);
        assert(ecat_foe_ssc_on_data(&adapter, fw_buf + 64U, 32U) == 0);
        assert(core.state == ECAT_FOE_STATE_SUCCESS);

        /* 外部调用 on_abort */
        ecat_foe_ssc_on_abort(&adapter, 0);
        assert(core.state == ECAT_FOE_STATE_SUCCESS);
        printf(" [PASS]\n");
    }

    /* [TEST 10] 空指针且非零长度包前置防御拦截 */
    {
        printf("[TEST 10] 验证 pData 为空指针且非零长度包被安全拦截 (返回 0x8004)...");
        setup_core_and_adapter(&core, &adapter, 64U);
        const char *valid_fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
        assert(ecat_foe_ssc_on_write(&adapter, valid_fn, (uint16_t)strlen(valid_fn), 0) == 0);

        uint16_t err = ecat_foe_ssc_on_data(&adapter, NULL, 32U);
        assert(err == ECAT_FOE_SSC_ERR_ILLEGAL); /* 0x8004 */
        assert(!adapter.session_active);
        printf(" [PASS]\n");
    }

    /* [TEST 11] WRQ 非法文件名错误码细分返回 0x8004 */
    {
        printf("[TEST 11] 验证 WRQ 非法文件名错误码细分映射为 0x8004 (ILLEGAL)...");
        setup_core_and_adapter(&core, &adapter, 64U);
        const char *bad_fn = "";
        uint16_t err = ecat_foe_ssc_on_write(&adapter, bad_fn, 0U, 0);
        assert(err == ECAT_FOE_SSC_ERR_ILLEGAL); /* 0x8004 */
        assert(!adapter.session_active);
        printf(" [PASS]\n");
    }

    printf("\n>>> Beckhoff SSC 适配层抽象契约 11 项测试全部通过 (Mock 环境) <<<\n");
    return 0;
}
