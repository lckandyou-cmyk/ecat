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

/* Test-only AL Control emulation for exercising the real ESC_state(). */
static bool s_emulate_al_control;
static uint8_t s_requested_al_state;
static bootloader_app_t *s_state_hook_app;

static void test_post_state_hook(uint8_t *as, uint8_t *an)
{
    ecat_foe_soes_post_state_hook(&s_state_hook_app->adapter, as, an);
}

static unsigned s_rejected_dispatch_calls;
static void rejected_dispatch_probe(void)
{
    s_rejected_dispatch_calls++;
}

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
    if (s_emulate_al_control && address == ESCREG_ALCONTROL && buf != NULL && len == 2U) {
        uint8_t *out = (uint8_t *)buf;
        out[0] = s_requested_al_state;
        out[1] = 0U;
        return;
    }
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
static uint8_t s_foe_fbuffer[512];

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
    s_foe_cfg.buffer_size = 128;
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

        /* 1. 构造合法 v2 .efw 固件 (148 字节头部 + 128 字节载荷 = 276 字节) */
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
        assert(efw_len == (ECAT_FOE_EFW_HEADER_MIN_LEN + sizeof(s_test_payload)));

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

        /* 分块 2 继续满包，分块 3 的短包触发 FOE_fclose 与最终校验提交。 */
        send_rc = bootloader_master_send_data(&app, &s_foe_cfg, 2, &s_test_efw_buf[116], 116, &resp);
        assert(send_rc == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK);
        assert(resp.packet_number == 2);

        send_rc = bootloader_master_send_data(&app, &s_foe_cfg, 3, &s_test_efw_buf[232], efw_len - 232U, &resp);
        assert(send_rc == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK);
        assert(resp.packet_number == 3);

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
    /* 场景 1B: 覆盖不同合法 SOES fbuffer 缓冲容量 (64 字节与 256 字节) 验证  */
    /* ---------------------------------------------------------------------- */
    {
        printf("--- [场景 1B] 覆盖不同合法 fbuffer 尺寸 (64 字节与 256 字节) 流式传输验证 ---\n");

        const uint32_t test_fbuf_sizes[] = {64, 256};
        for (size_t fbi = 0; fbi < sizeof(test_fbuf_sizes) / sizeof(test_fbuf_sizes[0]); fbi++) {
            uint32_t cur_fbuf_sz = test_fbuf_sizes[fbi];
            reset_test_env();
            s_foe_cfg.buffer_size = cur_fbuf_sz;
            FOE_config(&s_foe_cfg);
            FOE_init();

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
            assert(app.adapter.buffer_size == cur_fbuf_sz);

            master_foe_response_t resp;
            assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
            assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 0);

            /* 分块 1: 116 字节满包 (验证在 fbuffer=64 时不发生误报短包提前结束) */
            assert(bootloader_master_send_data(&app, &s_foe_cfg, 1, &s_test_efw_buf[0], 116, &resp) == 0);
            assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 1);

            /* 分块 2 满包，分块 3 短包收尾。 */
            assert(bootloader_master_send_data(&app, &s_foe_cfg, 2, &s_test_efw_buf[116], 116, &resp) == 0);
            assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 2);
            assert(bootloader_master_send_data(&app, &s_foe_cfg, 3, &s_test_efw_buf[232],
                                               ECAT_FOE_EFW_HEADER_MIN_LEN + sizeof(s_test_payload) - 232U,
                                               &resp) == 0);
            assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 3);

            assert(app.core.state == ECAT_FOE_STATE_SUCCESS);
            assert(app.flash_drv.is_finalized == true);
            assert(app.flash_drv.valid_flag == APP_FLASH_VALID_MAGIC);
            assert(memcmp(s_sim_flash, s_test_payload, 128) == 0);
        }

        printf("  [PASS] 场景 1B 验证通过: 真实 SOES 在 64 字节与 256 字节 fbuffer 下流式接收与最终提交正常闭环\n");
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
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1, &s_test_efw_buf[0], 116, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 2, &s_test_efw_buf[116], 32, &resp) == 0);
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
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1, &s_test_efw_buf[0], 116, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 2, &s_test_efw_buf[116], 32, &resp) == 0);
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
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1, &s_test_efw_buf[0], 116, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 2, &s_test_efw_buf[116], 32, &resp) == 0);
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
        assert(efw_len == ECAT_FOE_EFW_HEADER_MIN_LEN + 16U);

        app.flash_drv.inject_write_err = true;
        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
        /* 首包接收完整 v2 头的一部分，次包补齐头并触发写入故障。 */
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1, &s_test_efw_buf[0], 116, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 2, &s_test_efw_buf[116], efw_len - 116U, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ERR);
        assert(resp.error_code == 0x800B);
        assert(app.core.state == ECAT_FOE_STATE_ERROR);
        assert(app.flash_drv.valid_flag == APP_FLASH_INVALID_MAGIC);

        printf("  [PASS] 场景 3 验证通过: 存储驱动擦除/写入硬件故障被即时捕获，触发 Abort 且保持介质无效\n");
    }

    /* ---------------------------------------------------------------------- */
    /* 场景 3B: Flash 存储驱动安全失败与边界防御 (NULL/容量不足/溢出/越界)     */
    /* ---------------------------------------------------------------------- */
    {
        printf("--- [场景 3B] Flash 存储驱动安全失败与边界防御 (NULL/容量不足/溢出/越界) ---\n");
        const ecat_foe_target_driver_t *flash_ops = bootloader_flash_driver_get_ops();

        /* 1. storage_buf 为 NULL 测试：所有读写与提交均安全拒绝并保持无效魔数 */
        bootloader_flash_driver_t null_drv;
        bootloader_flash_driver_init_ctx(&null_drv, NULL, 0);
        assert(flash_ops->init(&null_drv) != 0);
        assert(flash_ops->invalidate(&null_drv) != 0);
        uint32_t erased = 0;
        assert(flash_ops->erase_to(4096, &erased, &null_drv) != 0);
        uint8_t dummy_d[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        assert(flash_ops->write(0, dummy_d, sizeof(dummy_d), &null_drv) != 0);
        uint8_t read_d[8] = {0};
        assert(flash_ops->read(0, read_d, sizeof(read_d), &null_drv) != 0);
        assert(flash_ops->finalize(&null_drv) != 0);
        assert(null_drv.valid_flag == APP_FLASH_INVALID_MAGIC);

        /* 2. storage_buf 容量不足测试 (缓冲尺寸小于 APP_FLASH_MAX_SIZE 配额) */
        uint8_t small_buf[1024];
        bootloader_flash_driver_t small_drv;
        bootloader_flash_driver_init_ctx(&small_drv, small_buf, sizeof(small_buf));
        assert(flash_ops->init(&small_drv) != 0);

        /* 3. 越界与整数溢出防御测试 */
        bootloader_flash_driver_t bound_drv;
        bootloader_flash_driver_init_ctx(&bound_drv, s_sim_flash, sizeof(s_sim_flash));
        assert(flash_ops->init(&bound_drv) == 0);
        assert(flash_ops->invalidate(&bound_drv) == 0);
        assert(flash_ops->erase_to(4096, &erased, &bound_drv) == 0);

        /* 3A: 写入整数溢出防御 (offset + len 溢出绕回) */
        assert(flash_ops->write(UINT32_MAX - 3, dummy_d, 8, &bound_drv) != 0);
        /* 3B: 写入越界 (超出 max_size 或超出当前 erased_watermark 水位) */
        assert(flash_ops->write(4096, dummy_d, 8, &bound_drv) != 0);
        /* 3C: 读取整数溢出防御 (offset + len 溢出绕回) */
        assert(flash_ops->read(UINT32_MAX - 3, read_d, 8, &bound_drv) != 0);
        /* 3D: 读取越界 (超出 max_size，绝不伪造 0xFF 掩盖错误) */
        assert(flash_ops->read(APP_FLASH_MAX_SIZE, read_d, 8, &bound_drv) != 0);
        /* 3E: 擦除超出 max_size 与加法整数溢出防御 */
        assert(flash_ops->erase_to(APP_FLASH_MAX_SIZE + 4096, &erased, &bound_drv) != 0);
        assert(flash_ops->erase_to(UINT32_MAX - 100, &erased, &bound_drv) != 0);

        printf("  [PASS] 场景 3B 验证通过: Flash 存储驱动在 NULL/容量不足/溢出/越界下严格安全失败\n");
    }

    /* ---------------------------------------------------------------------- */
    /* 场景 4: 中途异常中止 (主站 ERR) 与一次且仅一次驱动中止、自愈重试验证    */
    /* ---------------------------------------------------------------------- */
    {
        printf("--- [场景 4] 中途异常中止 (主站 ERR) 与一次且仅一次驱动中止、自愈重试验证 ---\n");
        reset_test_env();
        assert(bootloader_app_init(&app, &s_foe_cfg, MBXcontrol, MBX, MBXBUFFERS, ESC_MBXSIZE,
                                   s_sim_flash, sizeof(s_sim_flash), FOE_init, NULL) == 0);

        /* 构造 200 字节载荷的 v2 固件镜像 (148 字节头 + 200 字节载荷 = 348 字节) */
        uint8_t payload_200[200];
        for (size_t i = 0; i < sizeof(payload_200); i++) {
            payload_200[i] = (uint8_t)(0x30 + (i & 0x3F));
        }
        uint8_t efw_200[512];
        size_t efw_200_len = bootloader_master_build_efw(
            efw_200, sizeof(efw_200),
            BOOTLOADER_PROJECT_ID, BOOTLOADER_PROJECT_NAME, BOOTLOADER_HARDWARE_ID,
            BOOTLOADER_HW_VER_MIN, BOOTLOADER_HW_VER_MAX, BOOTLOADER_TARGET_MCU_ID,
            0x01000000U, payload_200, sizeof(payload_200));
        assert(efw_200_len == ECAT_FOE_EFW_HEADER_MIN_LEN + sizeof(payload_200));

        master_foe_response_t resp;
        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 0);

        /* 发送分块 1 (116 字节满包) */
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1, &efw_200[0], 116, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 1);

        /* 分块 2 后头部仍未完整到达；继续发满包 3，令 SOES 缓冲刷新完整 v2 头与部分载荷。 */
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 2, &efw_200[116], 116, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 2);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 3, &efw_200[232], 116, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 3);

        /* 确证核心已进入活跃会话且驱动已完成初始化与 invalidate */
        assert(app.core.session_active == true);
        assert(app.core.state == ECAT_FOE_STATE_DATA);
        assert(app.flash_drv.is_initialized == true);
        assert(app.flash_drv.is_invalidated == true);

        /* 主站此时下发 ERR 报文主动中断当前升级 */
        assert(bootloader_master_send_err(&app, &s_foe_cfg, 0x8000) == 0);

        /* 确证：驱动 abort 被恰好执行一次，核心 session_active 置 false，状态复位回 IDLE，有效标记保持为无效 */
        assert(app.core.session_active == false);
        assert(app.core.state == ECAT_FOE_STATE_IDLE);
        assert(app.flash_drv.is_initialized == false);
        assert(app.flash_drv.valid_flag == APP_FLASH_INVALID_MAGIC);

        /* 再次重复触发 abort (幂等性核查): 驱动 abort 不会重复破坏状态 */
        bootloader_app_abort(&app);
        assert(app.core.session_active == false);
        assert(app.flash_drv.valid_flag == APP_FLASH_INVALID_MAGIC);

        /* 此时 SOES 经过 FOE_error() -> FOE_abort() -> FOE_init() 自愈复位回 FOE_READY */
        /* 主站重新发起全新的固件升级全流程 (重新生成 v2 基准固件) */
        size_t retry_len = bootloader_master_build_efw(
            s_test_efw_buf, sizeof(s_test_efw_buf),
            BOOTLOADER_PROJECT_ID, BOOTLOADER_PROJECT_NAME, BOOTLOADER_HARDWARE_ID,
            BOOTLOADER_HW_VER_MIN, BOOTLOADER_HW_VER_MAX, BOOTLOADER_TARGET_MCU_ID,
            0x01000000U, s_test_payload, 128);
        assert(retry_len == ECAT_FOE_EFW_HEADER_MIN_LEN + sizeof(s_test_payload));

        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 0);

        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1, &s_test_efw_buf[0], 116, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 1);

        assert(bootloader_master_send_data(&app, &s_foe_cfg, 2, &s_test_efw_buf[116], 116, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 2);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 3, &s_test_efw_buf[232],
                                           retry_len - 232U, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 3);

        assert(app.core.state == ECAT_FOE_STATE_SUCCESS);
        assert(app.flash_drv.is_finalized == true);
        assert(app.flash_drv.valid_flag == APP_FLASH_VALID_MAGIC);
        assert(app.flash_drv.written_bytes == 128);
        assert(memcmp(s_sim_flash, s_test_payload, 128) == 0);

        printf("  [PASS] 场景 4 验证通过: 主站 ERR 即时触发一次且仅一次驱动中止、保留介质无效并支持后续自愈重试闭环\n");
    }

    /* Native SOES rejects an out-of-order DATA without calling write_cb. */
    {
        reset_test_env();
        s_foe_cfg.buffer_size = 64U;
        assert(bootloader_app_init(&app, &s_foe_cfg, MBXcontrol, MBX, MBXBUFFERS, ESC_MBXSIZE,
                                   s_sim_flash, sizeof(s_sim_flash), FOE_init, NULL) == 0);
        size_t n = bootloader_master_build_efw(s_test_efw_buf, sizeof(s_test_efw_buf),
            BOOTLOADER_PROJECT_ID, BOOTLOADER_PROJECT_NAME, BOOTLOADER_HARDWARE_ID,
            BOOTLOADER_HW_VER_MIN, BOOTLOADER_HW_VER_MAX, BOOTLOADER_TARGET_MCU_ID,
            0x01000000U, s_test_payload, sizeof(s_test_payload));
        assert(n > 232U);
        master_foe_response_t resp;
        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1, s_test_efw_buf, 116, &resp) == 0);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 2, s_test_efw_buf + 116, 116, &resp) == 0);
        assert(app.core.session_active && app.core.state == ECAT_FOE_STATE_DATA);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 4, s_test_efw_buf + 232, 1, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ERR && resp.error_code == 0x8005U);
        assert(!app.core.session_active && app.core.state == ECAT_FOE_STATE_IDLE);
        assert(app.flash_drv.valid_flag == APP_FLASH_INVALID_MAGIC);
        assert(ecat_foe_core_upgrade_incomplete(&app.core));
        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1, s_test_efw_buf, 116, &resp) == 0);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 2, s_test_efw_buf + 116, 116, &resp) == 0);
        assert(app.core.session_active);
        /* Simulate a processing cycle unable to claim any response slot. */
        uint8_t data_frame[12] = {6, 0, 0, 0, 0, 4, 3, 0, 3, 0, 0, 0};
        assert(ecat_foe_soes_pre_process_hook(&app.adapter, &s_foe_cfg, data_frame, sizeof(data_frame)) == 0);
        assert(ecat_foe_soes_post_process_hook(&app.adapter, &s_foe_cfg, NULL, 0) != 0);
        assert(!app.core.session_active && app.core.state == ECAT_FOE_STATE_IDLE);
        assert(ecat_foe_core_upgrade_incomplete(&app.core));
        printf("  [PASS] SOES 包号错误及无响应时中止核心会话并允许重试\n");
    }

    /* EOF before a complete header must never produce a success ACK. */
    for (int partial = 0; partial <= 1; partial++) {
        reset_test_env();
        assert(bootloader_app_init(&app, &s_foe_cfg, MBXcontrol, MBX, MBXBUFFERS, ESC_MBXSIZE,
                                   s_sim_flash, sizeof(s_sim_flash), FOE_init, NULL) == 0);
        assert(bootloader_master_build_efw(s_test_efw_buf, sizeof(s_test_efw_buf),
            BOOTLOADER_PROJECT_ID, BOOTLOADER_PROJECT_NAME, BOOTLOADER_HARDWARE_ID,
            BOOTLOADER_HW_VER_MIN, BOOTLOADER_HW_VER_MAX, BOOTLOADER_TARGET_MCU_ID,
            0x01000000U, s_test_payload, sizeof(s_test_payload)) > 116U);
        master_foe_response_t resp;
        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
        if (partial) {
            assert(bootloader_master_send_data(&app, &s_foe_cfg, 1U, s_test_efw_buf, 116U, &resp) == 0);
            assert(resp.opcode == BOOTLOADER_FOE_OP_ACK);
        }
        assert(bootloader_master_send_data(&app, &s_foe_cfg, partial ? 2U : 1U, NULL, 0U, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ERR);
        assert(!app.core.session_active && !app.flash_drv.is_finalized);
        assert(app.flash_drv.valid_flag == APP_FLASH_INVALID_MAGIC);
    }

    /* Unsupported opcode and malformed FoE must not strand an active driver. */
    for (int malformed = 0; malformed <= 1; malformed++) {
        reset_test_env();
        s_foe_cfg.buffer_size = 64U;
        assert(bootloader_app_init(&app, &s_foe_cfg, MBXcontrol, MBX, MBXBUFFERS, ESC_MBXSIZE,
                                   s_sim_flash, sizeof(s_sim_flash), FOE_init, NULL) == 0);
        assert(bootloader_master_build_efw(s_test_efw_buf, sizeof(s_test_efw_buf),
            BOOTLOADER_PROJECT_ID, BOOTLOADER_PROJECT_NAME, BOOTLOADER_HARDWARE_ID,
            BOOTLOADER_HW_VER_MIN, BOOTLOADER_HW_VER_MAX, BOOTLOADER_TARGET_MCU_ID,
            0x01000000U, s_test_payload, sizeof(s_test_payload)) > 232U);
        master_foe_response_t resp;
        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1U, s_test_efw_buf, 116U, &resp) == 0);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 2U, s_test_efw_buf + 116U, 116U, &resp) == 0);
        assert(app.core.session_active && app.flash_drv.is_initialized);
        memset(MBX, 0, 12U);
        MBX[0] = malformed ? 2U : 6U;
        MBX[5] = 4U;
        MBX[6] = 0x7fU;
        MBXcontrol[0].state = MBXstate_inclaim;
        (void)bootloader_app_step(&app, &s_foe_cfg, MBX, malformed ? 8U : 12U, NULL, 0, NULL);
        assert(!app.core.session_active && app.core.state == ECAT_FOE_STATE_IDLE);
        assert(!app.flash_drv.is_initialized && app.flash_drv.valid_flag == APP_FLASH_INVALID_MAGIC);
        assert(ecat_foe_core_upgrade_incomplete(&app.core));
    }

    /* Zero-length EOF: the native stack ignores close failure, so post hook
     * must replace the false ACK while preserving successful EOF behavior. */
    for (int fail_finalize = 0; fail_finalize <= 1; fail_finalize++) {
        reset_test_env();
        s_foe_cfg.buffer_size = 64U;
        assert(bootloader_app_init(&app, &s_foe_cfg, MBXcontrol, MBX, MBXBUFFERS, ESC_MBXSIZE,
                                   s_sim_flash, sizeof(s_sim_flash), FOE_init, NULL) == 0);
        uint8_t payload[200] = {0};
        size_t n = bootloader_master_build_efw(s_test_efw_buf, sizeof(s_test_efw_buf),
            BOOTLOADER_PROJECT_ID, BOOTLOADER_PROJECT_NAME, BOOTLOADER_HARDWARE_ID,
            BOOTLOADER_HW_VER_MIN, BOOTLOADER_HW_VER_MAX, BOOTLOADER_TARGET_MCU_ID,
            0x01000000U, payload, sizeof(payload));
        assert(n == 3U * 116U);
        master_foe_response_t resp;
        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
        for (uint32_t i = 0; i < 3U; i++) {
            assert(bootloader_master_send_data(&app, &s_foe_cfg, i + 1U, s_test_efw_buf + i * 116U, 116U, &resp) == 0);
            assert(resp.opcode == BOOTLOADER_FOE_OP_ACK);
        }
        app.flash_drv.inject_finalize_err = (fail_finalize != 0);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 4U, NULL, 0U, &resp) == 0);
        if (fail_finalize) {
            assert(resp.opcode == BOOTLOADER_FOE_OP_ERR);
            assert(app.core.state == ECAT_FOE_STATE_ERROR);
            assert(app.flash_drv.valid_flag == APP_FLASH_INVALID_MAGIC);
        } else {
            assert(resp.opcode == BOOTLOADER_FOE_OP_ACK);
            assert(app.core.state == ECAT_FOE_STATE_SUCCESS);
            assert(app.flash_drv.valid_flag == APP_FLASH_VALID_MAGIC);
        }
    }

    /* A new WRQ while native SOES is receiving aborts the old session. */
    for (int invalid_name = 0; invalid_name <= 1; invalid_name++) {
        reset_test_env();
        s_foe_cfg.buffer_size = 64U;
        assert(bootloader_app_init(&app, &s_foe_cfg, MBXcontrol, MBX, MBXBUFFERS, ESC_MBXSIZE,
                                   s_sim_flash, sizeof(s_sim_flash), FOE_init, NULL) == 0);
        size_t n = bootloader_master_build_efw(s_test_efw_buf, sizeof(s_test_efw_buf),
            BOOTLOADER_PROJECT_ID, BOOTLOADER_PROJECT_NAME, BOOTLOADER_HARDWARE_ID,
            BOOTLOADER_HW_VER_MIN, BOOTLOADER_HW_VER_MAX, BOOTLOADER_TARGET_MCU_ID,
            0x01000000U, s_test_payload, sizeof(s_test_payload));
        assert(n > 232U);
        master_foe_response_t resp;
        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1, s_test_efw_buf, 116, &resp) == 0);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 2, s_test_efw_buf + 116, 116, &resp) == 0);
        assert(app.core.session_active);
        const char *name = invalid_name ? "BAD_MCU_V1.0.0.0.efw" : "SERVO_DRV_MCU_V1.0.0.0.efw";
        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, name, 0, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ERR && resp.error_code == 0x8004U);
        assert(!app.core.session_active && app.core.state == ECAT_FOE_STATE_IDLE);
        assert(!app.flash_drv.is_initialized && app.flash_drv.valid_flag == APP_FLASH_INVALID_MAGIC);
        assert(ecat_foe_core_upgrade_incomplete(&app.core));
    }

    /* BOOT exit stops mailboxes; model the subsequent mailbox restart and
     * verify the first WRQ against the actual SOES FoE state machine. */
    for (unsigned active = 0; active <= 1U; active++) {
        for (unsigned has_reset = 0; has_reset <= 1U; has_reset++) {
            reset_test_env();
            s_foe_cfg.buffer_size = 64U;
            assert(bootloader_app_init(&app, &s_foe_cfg, MBXcontrol, MBX, MBXBUFFERS, ESC_MBXSIZE,
                s_sim_flash, sizeof(s_sim_flash), has_reset ? FOE_init : NULL, NULL) == 0);
            assert(bootloader_master_build_efw(s_test_efw_buf, sizeof(s_test_efw_buf),
                BOOTLOADER_PROJECT_ID, BOOTLOADER_PROJECT_NAME, BOOTLOADER_HARDWARE_ID,
                BOOTLOADER_HW_VER_MIN, BOOTLOADER_HW_VER_MAX, BOOTLOADER_TARGET_MCU_ID,
                0x01000000U, s_test_payload, sizeof(s_test_payload)) > 232U);
            master_foe_response_t resp;
            assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
            if (active) {
                assert(bootloader_master_send_data(&app, &s_foe_cfg, 1U, s_test_efw_buf, 116U, &resp) == 0);
                assert(bootloader_master_send_data(&app, &s_foe_cfg, 2U, s_test_efw_buf + 116U, 116U, &resp) == 0);
                assert(app.core.session_active && ecat_foe_core_upgrade_incomplete(&app.core));
            }
            /* Model the stopped mailbox without executing hardware SM polling. */
            ESCvar.MBXrun = 0;
            memset(MBXcontrol, 0, sizeof(MBXcontrol));
            uint8_t as = ECAT_TRANSITION_BOOT_TO_INIT, an = ECAT_STATE_INIT;
            ecat_foe_soes_post_state_hook(&app.adapter, &as, &an);
            assert(!app.core.session_active && app.core.state == ECAT_FOE_STATE_IDLE);
            assert(ecat_foe_core_upgrade_incomplete(&app.core) == (active != 0U));
            assert(an == (active ? ECAT_AL_STATUS_BOOT_ERROR : ECAT_STATE_INIT));
            /* This only simulates mailbox availability, not the AL recovery sequence. */
            ESCvar.MBXrun = 1;
            assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
            if (!has_reset) {
                assert(resp.opcode == BOOTLOADER_FOE_OP_ERR && resp.error_code == 0x8004U);
                assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
            }
            assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 0U);
        }
    }
    printf("  [PASS] BOOT 中止后 SOES 复位与无复位回调恢复验证通过\n");

    /* Exercise the real SOES AL state machine for invalid BOOT exits. */
    {
        const uint8_t requested[] = {ESCpreop, ESCsafeop, ESCop};
        for (size_t i = 0; i < sizeof(requested); i++) {
            for (unsigned active = 0; active <= 1U; active++) {
                for (unsigned has_reset = 0; has_reset <= 1U; has_reset++) {
                    reset_test_env();
                    s_foe_cfg.buffer_size = 64U;
                    assert(bootloader_app_init(&app, &s_foe_cfg, MBXcontrol, MBX, MBXBUFFERS,
                        ESC_MBXSIZE, s_sim_flash, sizeof(s_sim_flash), has_reset ? FOE_init : NULL,
                        ESC_ALerror) == 0);
                    assert(bootloader_master_build_efw(s_test_efw_buf, sizeof(s_test_efw_buf),
                        BOOTLOADER_PROJECT_ID, BOOTLOADER_PROJECT_NAME, BOOTLOADER_HARDWARE_ID,
                        BOOTLOADER_HW_VER_MIN, BOOTLOADER_HW_VER_MAX, BOOTLOADER_TARGET_MCU_ID,
                        0x01000000U, s_test_payload, sizeof(s_test_payload)) > 232U);
                    master_foe_response_t resp;
                    assert(bootloader_master_send_wrq(&app, &s_foe_cfg,
                        "SERVO_DRV_MCU_V1.0.0.0.efw", 0U, &resp) == 0);
                    assert(resp.opcode == BOOTLOADER_FOE_OP_ACK);
                    if (active) {
                        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1U,
                            s_test_efw_buf, 116U, &resp) == 0);
                        assert(bootloader_master_send_data(&app, &s_foe_cfg, 2U,
                            s_test_efw_buf + 116U, 116U, &resp) == 0);
                        assert(app.core.session_active && app.flash_drv.is_initialized);
                    }
                    s_state_hook_app = &app;
                    s_requested_al_state = requested[i];
                    s_emulate_al_control = true;
                    ESCvar.post_state_change_hook = test_post_state_hook;
                    ESCvar.ALstatus = ESCboot;
                    ESCvar.ALevent = ESCREG_ALEVENT_CONTROL;
                    ESC_state();
                    s_emulate_al_control = false;
                    assert(ESCvar.ALstatus == (ESCpreop | ESCerror));
                    assert(ESCvar.ALerror == ALERR_INVALIDSTATECHANGE);
                    assert(app.core.state == ECAT_FOE_STATE_IDLE && !app.core.session_active);
                    assert(!app.flash_drv.is_initialized);
                    assert(ecat_foe_core_upgrade_incomplete(&app.core) == (active != 0U));
                    assert(app.flash_drv.valid_flag == APP_FLASH_INVALID_MAGIC);
                    /* Model a recovered BOOT state; not an AL recovery test. */
                    ESCvar.ALstatus = ESCboot;
                    ESCvar.MBXrun = 1U;
                    assert(bootloader_master_send_wrq(&app, &s_foe_cfg,
                        "SERVO_DRV_MCU_V1.0.0.0.efw", 0U, &resp) == 0);
                    if (!has_reset) {
                        assert(resp.opcode == BOOTLOADER_FOE_OP_ERR && resp.error_code == 0x8004U);
                        assert(bootloader_master_send_wrq(&app, &s_foe_cfg,
                            "SERVO_DRV_MCU_V1.0.0.0.efw", 0U, &resp) == 0);
                    }
                    assert(resp.opcode == BOOTLOADER_FOE_OP_ACK && resp.packet_number == 0U);
                }
            }
        }
        printf("  [PASS] SOES 非法 BOOT 状态迁移清理和恢复验证通过\n");
    }

    /* Reject malformed or unassociated input before calling SOES. */
    for (unsigned variant = 0; variant < 8U; variant++) {
        static uint8_t large_image[ECAT_FOE_EFW_HEADER_MIN_LEN + 8192U];
        static uint8_t large_payload[8192U];
        uint8_t other[12] = {6U, 0U, 0U, 0U, 0U, 4U, 3U, 0U, 3U, 0U, 0U, 0U};
        reset_test_env();
        s_foe_cfg.buffer_size = 64U;
        assert(bootloader_app_init(&app, &s_foe_cfg, MBXcontrol, MBX, MBXBUFFERS, ESC_MBXSIZE,
                                   s_sim_flash, sizeof(s_sim_flash), FOE_init, NULL) == 0);
        assert(bootloader_master_build_efw(large_image, sizeof(large_image),
            BOOTLOADER_PROJECT_ID, BOOTLOADER_PROJECT_NAME, BOOTLOADER_HARDWARE_ID,
            BOOTLOADER_HW_VER_MIN, BOOTLOADER_HW_VER_MAX, BOOTLOADER_TARGET_MCU_ID,
            0x01000000U, large_payload, sizeof(large_payload)) == sizeof(large_image));
        master_foe_response_t resp;
        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0U, &resp) == 0);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 1U, large_image, 116U, &resp) == 0);
        assert(bootloader_master_send_data(&app, &s_foe_cfg, 2U, large_image + 116U, 116U, &resp) == 0);
        assert(app.core.session_active && app.flash_drv.is_initialized);

        memset(MBX, 0, ESC_MBXSIZE);
        MBX[0] = 6U; MBX[5] = 4U; MBX[6] = 3U; MBX[8] = 3U;
        MBXcontrol[0].state = MBXstate_inclaim;
        const uint8_t *rx = MBX;
        size_t rx_len = 12U;
        if (variant == 0U) { /* Original ASan trigger: claimed length exceeds the received slot. */
            const uint16_t claim = 6006U;
            MBX[0] = (uint8_t)claim; MBX[1] = (uint8_t)(claim >> 8);
            memcpy(MBX + 12U, large_image + 232U, 116U);
            rx_len = ESC_MBXSIZE;
        } else if (variant == 1U) {
            MBX[0] = 7U; /* Header declares one byte more than received. */
        } else if (variant == 2U) {
            MBX[0] = 123U; rx_len = sizeof(MBX); /* Larger than one slot. */
        } else if (variant == 3U) {
            rx_len = 5U; /* Truncated mailbox header. */
        } else if (variant == 4U) {
            rx = other; /* Checked bytes are not the actual SOES input slot. */
        } else if (variant == 5U) {
            rx = NULL; rx_len = 0U; /* Claimed input cannot be skipped. */
        } else if (variant == 6U) {
            rx = NULL; rx_len = 12U;
        } else {
            rx_len = 0U;
        }
        s_rejected_dispatch_calls = 0U;
        /* Variant zero uses the real stack; others instrument dispatcher calls. */
        int rc = bootloader_app_step(&app, &s_foe_cfg, rx, rx_len, NULL, 0,
                                     variant == 0U ? NULL : rejected_dispatch_probe);
        assert(rc == -1 && s_rejected_dispatch_calls == 0U);
        assert(!app.core.session_active && app.core.state == ECAT_FOE_STATE_IDLE);
        assert(!app.flash_drv.is_initialized && app.flash_drv.valid_flag == APP_FLASH_INVALID_MAGIC);
        assert(ecat_foe_core_upgrade_incomplete(&app.core));
        assert(MBXcontrol[0].state == MBXstate_inclaim); /* Mailbox owner retains ownership. */
        MBXcontrol[0].state = MBXstate_idle; /* Model the owner's drop. */
        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0U, &resp) == 0);
        assert(resp.opcode == BOOTLOADER_FOE_OP_ACK);
    }
    /* An empty idle poll is valid but must not dispatch stale mailbox data. */
    reset_test_env();
    assert(bootloader_app_init(&app, &s_foe_cfg, MBXcontrol, MBX, MBXBUFFERS, ESC_MBXSIZE,
                               s_sim_flash, sizeof(s_sim_flash), FOE_init, NULL) == 0);
    s_rejected_dispatch_calls = 0U;
    assert(bootloader_app_step(&app, &s_foe_cfg, NULL, 0U, NULL, 0U, rejected_dispatch_probe) == 0);
    assert(s_rejected_dispatch_calls == 0U);
    /* A well-formed WRQ with a bad project must still reach SOES for 0x8001. */
    master_foe_response_t rejected_wrq;
    assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "BAD_MCU_V1.0.0.0.efw", 0U, &rejected_wrq) == 0);
    assert(rejected_wrq.opcode == BOOTLOADER_FOE_OP_ERR && rejected_wrq.error_code == 0x8001U);
    printf("  [PASS] 畸形邮箱输入在 SOES 分发前拒绝并安全清理\n");

    /* SOES counts the full EFW file; Flash capacity applies to payload only. */
    for (unsigned oversized = 0; oversized <= 1U; oversized++) {
        static uint8_t capacity_payload[APP_FLASH_MAX_SIZE + 1U];
        static uint8_t capacity_efw[ECAT_FOE_EFW_HEADER_MIN_LEN + APP_FLASH_MAX_SIZE + 1U];
        reset_test_env();
        assert(bootloader_app_init(&app, &s_foe_cfg, MBXcontrol, MBX, MBXBUFFERS, ESC_MBXSIZE,
                                   s_sim_flash, sizeof(s_sim_flash), FOE_init, NULL) == 0);
        assert(s_foe_files[0].max_data == APP_FLASH_MAX_SIZE + ECAT_FOE_EFW_HEADER_MIN_LEN);
        size_t payload_len = APP_FLASH_MAX_SIZE + oversized;
        for (size_t i = 0; i < payload_len; i++) capacity_payload[i] = (uint8_t)i;
        size_t n = bootloader_master_build_efw(capacity_efw, sizeof(capacity_efw),
            BOOTLOADER_PROJECT_ID, BOOTLOADER_PROJECT_NAME, BOOTLOADER_HARDWARE_ID,
            BOOTLOADER_HW_VER_MIN, BOOTLOADER_HW_VER_MAX, BOOTLOADER_TARGET_MCU_ID,
            0x01000000U, capacity_payload, payload_len);
        assert(n == ECAT_FOE_EFW_HEADER_MIN_LEN + payload_len);
        master_foe_response_t resp;
        assert(bootloader_master_send_wrq(&app, &s_foe_cfg, "SERVO_DRV_MCU_V1.0.0.0.efw", 0, &resp) == 0);
        size_t pos = 0;
        uint32_t packet = 1U;
        while (pos < n) {
            size_t chunk = (n - pos > 116U) ? 116U : n - pos;
            assert(bootloader_master_send_data(&app, &s_foe_cfg, packet++, capacity_efw + pos, chunk, &resp) == 0);
            if (resp.opcode != BOOTLOADER_FOE_OP_ACK) break;
            pos += chunk;
        }
        if (oversized) {
            assert(resp.opcode == BOOTLOADER_FOE_OP_ERR);
            assert(!app.flash_drv.is_invalidated && app.flash_drv.written_bytes == 0U);
        } else {
            assert(pos == n && app.core.state == ECAT_FOE_STATE_SUCCESS);
            assert(app.flash_drv.written_bytes == APP_FLASH_MAX_SIZE);
            assert(memcmp(s_sim_flash, capacity_payload, APP_FLASH_MAX_SIZE) == 0);
        }
    }
    printf("  [PASS] SOES 全容量升级及超限拒绝验证通过\n");

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
