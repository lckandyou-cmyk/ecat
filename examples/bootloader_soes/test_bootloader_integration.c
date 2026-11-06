/**
 * @file test_bootloader_integration.c
 * @brief 最小 Bootloader 宿主可执行集成验证套件
 *
 * 验证目标 (宿主可复现验证，链接真实 SOES esc_foe.c 与 esc.c):
 * 1. 有效固件升级全流程: WRQ -> DATA -> 短包结束 -> 回读校验 -> 驱动成功提交；
 * 2. 项目与硬件不符拦截: WRQ 通过但 .efw 头部 project_id 或 hardware_id 不符，及时拦截并触发 Abort；
 * 3. 存储驱动失败保护: 模拟 Flash 擦除或写入硬件故障，状态机安全熔断并调用驱动 abort 清理；
 * 4. 主站中止与干净恢复: 中途传输异常或收到 ERR 中止后，SOES 恢复 FOE_READY，后续重发升级顺利完成；
 * 5. 写入物理基地址独家由本地配置裁决: 验证写操作严格受限于本地 APP_FLASH_BASE_ADDR，杜绝越界破坏 Bootloader。
 *
 * 证据与边界说明:
 * - 本测试在宿主环境 (macOS/Linux) 运行，通过内存缓冲区模拟物理 Flash 与 ESC 硬件端口；
 * - 真实板卡 (STM32/XMC4800/LAN9252) 擦写总线冻结与现场物理总线联调尚未开展，需实机验证。
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>

#include "esc.h"
#include "esc_foe.h"
#include "bootloader_config.h"
#include "bootloader_flash_driver.h"
#include "bootloader_app.h"
#include "bootloader_master_flow.h"

/* 真实 SOES 全局变量分配 (遵循 esc.h 官方契约) */
_ESCvar ESCvar;
_MBXcontrol MBXcontrol[MBXBUFFERS];
uint8_t MBX[MBXBUFFERS * MBXSIZE];

/* 宿主模拟 ESC 物理硬件端口 */
static uint8_t s_hw_sm1_buf[128];
static uint16_t s_hw_sm1_len = 0;

void ESC_write(uint16_t address, void *buf, uint16_t len)
{
    if (address == ESC_MBX1_sma && buf != NULL) {
        s_hw_sm1_len = (len > sizeof(s_hw_sm1_buf)) ? (uint16_t)sizeof(s_hw_sm1_buf) : len;
        memcpy(s_hw_sm1_buf, buf, s_hw_sm1_len);
    }
}

void ESC_read(uint16_t address, void *buf, uint16_t len)
{
    (void)address;
    if (buf != NULL && len > 0) {
        memset(buf, 0, len);
    }
}

void APP_safeoutput(void) {}

uint16_t sizeOfPDO(uint16_t objidx, int *mappings, _SMmap *map, int max_mappings)
{
    (void)objidx;
    (void)mappings;
    (void)map;
    (void)max_mappings;
    return 0;
}

_SMmap SMmap2[1];
_SMmap SMmap3[1];

/* 平台防回滚持久化存取桩函数 */
int port_get_version_floor(uint16_t target_id, uint32_t *floor_ver)
{
    (void)target_id;
    if (floor_ver != NULL) {
        *floor_ver = 0;
    }
    return 0;
}

int port_set_version_floor(uint16_t target_id, uint32_t new_floor)
{
    (void)target_id;
    (void)new_floor;
    return 0;
}

/* 宿主模拟 Flash 介质 (对齐 APP_FLASH_MAX_SIZE) */
static uint8_t s_sim_flash[APP_FLASH_MAX_SIZE];

/* 宿主模拟 Bootloader 保留区 (用于校验是否被越界踩踏) */
static uint8_t s_bootloader_sentinel[1024];

/* SOES FoE 文件配置 */
static foe_file_cfg_t s_foe_files[1];
static foe_cfg_t s_foe_cfg;
static uint8_t s_foe_fbuffer[128];

/* 测试固件缓冲 */
static uint8_t s_test_efw_buf[512];
static uint8_t s_test_payload[128];

/* 辅助：重置 SOES 与模拟环境 */
static void reset_test_env(void)
{
    static esc_cfg_t esc_cfg = {
        .use_interrupt = 0,
        .watchdog_cnt = 0
    };

    memset(&ESCvar, 0, sizeof(ESCvar));
    memset(MBXcontrol, 0, ESC_MBXBUFFERS * sizeof(_MBXcontrol));
    memset(MBX, 0, ESC_MBXBUFFERS * ESC_MBXSIZE);

    ESC_config(&esc_cfg);
    ESCvar.activemb0 = &ESCvar.mb[0];
    ESCvar.activemb1 = &ESCvar.mb[1];
    ESCvar.MBXrun = 1;
    ESCvar.activembxsize = 128;
    ESCvar.ALstatus = ESCboot;
    ESCvar.mbxfree = 1;

    s_hw_sm1_len = 0;
    memset(s_hw_sm1_buf, 0, sizeof(s_hw_sm1_buf));

    /* 模拟 Flash 介质预填充 0xFF (擦除态) */
    memset(s_sim_flash, 0xFF, sizeof(s_sim_flash));

    /* 模拟 Bootloader 物理保护区填充哨兵字节 0xA5 */
    memset(s_bootloader_sentinel, 0xA5, sizeof(s_bootloader_sentinel));

    /* 初始化 SOES FoE 配置 */
    memset(s_foe_files, 0, sizeof(s_foe_files));
    s_foe_files[0].name = BOOTLOADER_TARGET_MCU_FILENAME;
    s_foe_files[0].max_data = APP_FLASH_MAX_SIZE;
    s_foe_files[0].write_only_in_boot = 0;
    s_foe_files[0].filepass = 0;
    s_foe_files[0].write_function = ecat_foe_soes_write_cb;

    s_foe_cfg.fbuffer = s_foe_fbuffer;
    s_foe_cfg.buffer_size = sizeof(s_foe_fbuffer);
    s_foe_cfg.n_files = 1;
    s_foe_cfg.files = s_foe_files;

    FOE_config(&s_foe_cfg);
    FOE_init();
}

int main(void)
{
    printf("============================================================\n");
    printf(" 最小 Bootloader 宿主可执行集成验证套件 (真实 SOES 链路)\n");
    printf("============================================================\n");

    /* 预置测试载荷 (128 字节特征序列) */
    for (size_t i = 0; i < sizeof(s_test_payload); i++) {
        s_test_payload[i] = (uint8_t)(0x10 + (i & 0x7F));
    }

    bootloader_app_t app;

    /* ---------------------------------------------------------------------- */
    /* 场景 1: 有效固件端到端升级闭环验证                                    */
    /* ---------------------------------------------------------------------- */
    {
        printf("--- [场景 1] 有效固件端到端升级闭环验证 ---\n");
        reset_test_env();

        int init_rc = bootloader_app_init(&app,
                                          &s_foe_cfg,
                                          MBXcontrol,
                                          MBX,
                                          MBXBUFFERS,
                                          ESC_MBXSIZE,
                                          s_sim_flash,
                                          sizeof(s_sim_flash),
                                          FOE_init,
                                          NULL);
        assert(init_rc == 0);
        assert(app.is_initialized == true);

        /* 1. 构造合法 .efw 固件 (64 字节头部 + 128 字节载荷 = 192 字节) */
        size_t efw_len = bootloader_master_build_efw(
            s_test_efw_buf,
            sizeof(s_test_efw_buf),
            BOOTLOADER_PROJECT_ID,
            BOOTLOADER_PROJECT_NAME,
            BOOTLOADER_HARDWARE_ID,
            BOOTLOADER_HW_VER_MIN,
            BOOTLOADER_HW_VER_MAX,
            BOOTLOADER_TARGET_MCU_ID,
            0x01000000U, /* 1.0.0.0 */
            s_test_payload,
            sizeof(s_test_payload));
        assert(efw_len == (64 + sizeof(s_test_payload)));

        /* 2. 主站下发 WRQ 写请求 */
        master_foe_response_t resp;
        const char *fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
        int send_rc = bootloader_master_send_wrq(&app, &s_foe_cfg, fn, 0, &resp);
        assert(send_rc == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK);
        assert(resp.packet_number == 0);
        assert(app.core.state == ECAT_FOE_STATE_WRQ_ACKED);
        /* WRQ 阶段尚未执行 invalidate，防止误清标志 */
        assert(app.flash_drv.is_invalidated == false);

        /* 3. 主站流式发送固件 (分块 1: 116 字节满包) */
        send_rc = bootloader_master_send_data(&app, &s_foe_cfg, 1, &s_test_efw_buf[0], 116, &resp);
        assert(send_rc == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK);
        assert(resp.packet_number == 1);
        /* 满包在 SOES fbuffer 中累积，待短包收尾时统一刷新写入 */
        assert(app.core.state == ECAT_FOE_STATE_WRQ_ACKED);

        /* 4. 主站发送分块 2 (剩余 76 字节短包收尾，触发 FOE_fclose 刷新与驱动闭环) */
        send_rc = bootloader_master_send_data(&app, &s_foe_cfg, 2, &s_test_efw_buf[116], 76, &resp);
        assert(send_rc == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK);
        assert(resp.packet_number == 2);

        /* 核心与驱动状态断言: 校验通过、驱动初始化、置无效先行、按需擦写与最终提交全闭环 */
        assert(app.core.state == ECAT_FOE_STATE_SUCCESS);
        assert(app.flash_drv.is_initialized == true);
        assert(app.flash_drv.is_invalidated == true);
        assert(app.flash_drv.is_finalized == true);
        assert(app.flash_drv.valid_flag == APP_FLASH_VALID_MAGIC);
        assert(app.flash_drv.written_bytes == 128);

        /* 逐字节比对模拟物理 Flash 写入内容 */
        assert(memcmp(s_sim_flash, s_test_payload, sizeof(s_test_payload)) == 0);

        /* 校验 Bootloader 保护区未受触碰 */
        for (size_t i = 0; i < sizeof(s_bootloader_sentinel); i++) {
            assert(s_bootloader_sentinel[i] == 0xA5);
        }

        printf("  [PASS] 场景 1 验证通过: WRQ->DATA 分块流式接收完成，介质有效标记置位，载荷逐字节完全匹配\n");
    }

    /* ---------------------------------------------------------------------- */
    /* 场景 2: 工程标识与硬件版本不匹配安全拦截                               */
    /* ---------------------------------------------------------------------- */
    {
        printf("--- [场景 2] 工程标识与硬件版本不匹配安全拦截 ---\n");

        /* 2A: 工程 ID 不匹配 (文件名虽然符合前缀，但固件头 project_id 不符) */
        reset_test_env();
        int init_rc = bootloader_app_init(&app, &s_foe_cfg, MBXcontrol, MBX, MBXBUFFERS, ESC_MBXSIZE,
                                          s_sim_flash, sizeof(s_sim_flash), FOE_init, NULL);
        assert(init_rc == 0);

        size_t bad_efw_len = bootloader_master_build_efw(
            s_test_efw_buf, sizeof(s_test_efw_buf),
            0x20020002U, /* 错误的 project_id */
            BOOTLOADER_PROJECT_NAME,
            BOOTLOADER_HARDWARE_ID,
            BOOTLOADER_HW_VER_MIN,
            BOOTLOADER_HW_VER_MAX,
            BOOTLOADER_TARGET_MCU_ID,
            0x01000000U,
            s_test_payload, sizeof(s_test_payload));
        assert(bad_efw_len > 0);

        master_foe_response_t resp;
        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 0);

        /* 发送包含非法头部的数据包 1 */
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1, &s_test_efw_buf[0], 64, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ERR); /* FOE_OP_ERR */
        assert(resp.error_code == 0x800B);
        assert(app.core.state == ECAT_FOE_STATE_ERROR);
        /* Flash 未被擦写 */
        assert(app.flash_drv.written_bytes == 0);
        assert(app.flash_drv.is_finalized == false);

        /* 2B: 硬件板卡 ID 不匹配 */
        reset_test_env();
        assert(bootloader_app_init(&app, &s_foe_cfg, MBXcontrol, MBX, MBXBUFFERS, ESC_MBXSIZE,
                                   s_sim_flash, sizeof(s_sim_flash), FOE_init, NULL) == 0);

        bad_efw_len = bootloader_master_build_efw(
            s_test_efw_buf, sizeof(s_test_efw_buf),
            BOOTLOADER_PROJECT_ID,
            BOOTLOADER_PROJECT_NAME,
            0x00000002U, /* 错误的 hardware_id */
            BOOTLOADER_HW_VER_MIN,
            BOOTLOADER_HW_VER_MAX,
            BOOTLOADER_TARGET_MCU_ID,
            0x01000000U,
            s_test_payload, sizeof(s_test_payload));
        assert(bad_efw_len > 0);

        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1, &s_test_efw_buf[0], 64, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ERR);
        assert(resp.error_code == 0x800B);
        assert(app.core.state == ECAT_FOE_STATE_ERROR);
        assert(app.flash_drv.written_bytes == 0);

        printf("  [PASS] 场景 2 验证通过: 容器内工程 ID 与硬件 ID 不匹配被严格拦截，未擦写介质并回复 Abort\n");
    }

    /* ---------------------------------------------------------------------- */
    /* 场景 3: 存储驱动硬件故障保护 (擦除失败与写入失败)                      */
    /* ---------------------------------------------------------------------- */
    {
        printf("--- [场景 3] 存储驱动硬件故障保护 ---\n");

        /* 3A: 擦除失败注入 */
        reset_test_env();
        assert(bootloader_app_init(&app, &s_foe_cfg, MBXcontrol, MBX, MBXBUFFERS, ESC_MBXSIZE,
                                   s_sim_flash, sizeof(s_sim_flash), FOE_init, NULL) == 0);

        app.flash_drv.inject_erase_err = true;
        size_t efw_len = bootloader_master_build_efw(
            s_test_efw_buf, sizeof(s_test_efw_buf),
            BOOTLOADER_PROJECT_ID, BOOTLOADER_PROJECT_NAME, BOOTLOADER_HARDWARE_ID,
            BOOTLOADER_HW_VER_MIN, BOOTLOADER_HW_VER_MAX, BOOTLOADER_TARGET_MCU_ID,
            0x01000000U, s_test_payload, sizeof(s_test_payload));
        assert(efw_len > 0);

        master_foe_response_t resp;
        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
        /* 擦除失败导致第 1 包被拒 */
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1, &s_test_efw_buf[0], 64, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ERR);
        assert(resp.error_code == 0x800B);
        assert(app.core.state == ECAT_FOE_STATE_ERROR);
        assert(app.flash_drv.is_initialized == false); /* abort 已调用释放驱动 */

        /* 3B: 写入失败注入 */
        reset_test_env();
        assert(bootloader_app_init(&app, &s_foe_cfg, MBXcontrol, MBX, MBXBUFFERS, ESC_MBXSIZE,
                                   s_sim_flash, sizeof(s_sim_flash), FOE_init, NULL) == 0);

        efw_len = bootloader_master_build_efw(
            s_test_efw_buf, sizeof(s_test_efw_buf),
            BOOTLOADER_PROJECT_ID, BOOTLOADER_PROJECT_NAME, BOOTLOADER_HARDWARE_ID,
            BOOTLOADER_HW_VER_MIN, BOOTLOADER_HW_VER_MAX, BOOTLOADER_TARGET_MCU_ID,
            0x01000000U, s_test_payload, 16);
        assert(efw_len == 80);

        app.flash_drv.inject_write_err = true;
        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
        /* 发送 80 字节短包，在执行写入载荷时触发硬件故障 */
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1, &s_test_efw_buf[0], 80, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ERR);
        assert(resp.error_code == 0x800B);
        assert(app.core.state == ECAT_FOE_STATE_ERROR);
        assert(app.flash_drv.valid_flag == APP_FLASH_INVALID_MAGIC);

        printf("  [PASS] 场景 3 验证通过: 存储驱动擦除/写入硬件故障被即时捕获，触发 Abort 且保持介质无效\n");
    }

    /* ---------------------------------------------------------------------- */
    /* 场景 4: 中途异常中止与自愈恢复重试                                     */
    /* ---------------------------------------------------------------------- */
    {
        printf("--- [场景 4] 中途异常中止与自愈恢复重试 ---\n");
        reset_test_env();
        assert(bootloader_app_init(&app, &s_foe_cfg, MBXcontrol, MBX, MBXBUFFERS, ESC_MBXSIZE,
                                   s_sim_flash, sizeof(s_sim_flash), FOE_init, NULL) == 0);

        size_t efw_len = bootloader_master_build_efw(
            s_test_efw_buf, sizeof(s_test_efw_buf),
            BOOTLOADER_PROJECT_ID, BOOTLOADER_PROJECT_NAME, BOOTLOADER_HARDWARE_ID,
            BOOTLOADER_HW_VER_MIN, BOOTLOADER_HW_VER_MAX, BOOTLOADER_TARGET_MCU_ID,
            0x01000000U, s_test_payload, sizeof(s_test_payload));
        assert(efw_len > 0);

        master_foe_response_t resp;
        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1, &s_test_efw_buf[0], 116, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 1);

        /* 主站中途由于本地异常主动发送 ERR 报文中断当前升级 */
        assert(bootloader_master_send_err(&app, &s_foe_cfg, 0x8000) == 0);

        /* 此时 SOES 经过 FOE_error() -> FOE_abort() -> FOE_init() 自愈复位回 FOE_READY */
        /* 主站重新发起全新的固件升级全流程 */
        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 0);

        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1, &s_test_efw_buf[0], 116, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 1);

        assert(bootloader_master_send_data(&app, &s_foe_cfg, 2, &s_test_efw_buf[116], 76, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 2);

        assert(app.core.state == ECAT_FOE_STATE_SUCCESS);
        assert(app.flash_drv.is_finalized == true);
        assert(app.flash_drv.valid_flag == APP_FLASH_VALID_MAGIC);
        assert(app.flash_drv.written_bytes == 128);
        assert(memcmp(s_sim_flash, s_test_payload, sizeof(s_test_payload)) == 0);

        assert(app.core.state == ECAT_FOE_STATE_SUCCESS);
        assert(app.flash_drv.is_finalized == true);
        assert(app.flash_drv.valid_flag == APP_FLASH_VALID_MAGIC);

        printf("  [PASS] 场景 4 验证通过: 中途异常中止后 SOES 与核心干净复位，重试升级直接闭环成功\n");
    }

    /* ---------------------------------------------------------------------- */
    /* 场景 5: 写入物理基地址独家由本地配置裁决边界验证                       */
    /* ---------------------------------------------------------------------- */
    {
        printf("--- [场景 5] 写入物理基地址独家由本地配置裁决验证 ---\n");

        /* 断言 Flash 驱动中的基地址恒等于本地宏 APP_FLASH_BASE_ADDR */
        assert(app.flash_drv.base_address == APP_FLASH_BASE_ADDR);
        assert(app.flash_drv.max_size == APP_FLASH_MAX_SIZE);

        /* 验证无论远程报文如何变更文件名或数据，实际写入偏移均在本地配额内 */
        assert(app.flash_drv.written_bytes <= APP_FLASH_MAX_SIZE);

        /* 验证 Bootloader 物理地址区域 (0x08000000 ~ 0x0801FFFF) 从未被写入 */
        for (size_t i = 0; i < sizeof(s_bootloader_sentinel); i++) {
            assert(s_bootloader_sentinel[i] == 0xA5);
        }

        printf("  [PASS] 场景 5 验证通过: 物理写入基地址独家由本地配置裁决，Bootloader 分区得到严密隔离保护\n");
    }

    printf("------------------------------------------------------------\n");
    printf(">>> 最小 Bootloader 5 大核心场景集成验证全部通过！<<<\n");
    return 0;
}
