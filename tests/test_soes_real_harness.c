/**
 * @file test_soes_real_harness.c
 * @brief 真实 SOES esc_foe.c 宿主独立测试 Harness
 *
 * 验证目标 (严格区分两种证据类型):
 * 1. 首块回调时机核查: 验证真实 SOES 在 WRQ 阶段是否触发 write_function；
 * 2. 文件名前缀匹配核查: 验证真实 SOES FOE_fopen 中 strncmp(name, cfg_name, num_chars) 的前缀匹配机理；
 * 3. 零长度包返回值核查: 验证真实 SOES 在 data_len == 0 分支对 FOE_fclose() 返回值的忽略特性与短包 abort 行为对比；
 * 4. 孤立直接调用防御 (证据 1): 独立单元测试直接调用 core_on_data 与 write_cb，验证未经 WRQ 建立会话时的接口级防御；
 * 5. 真实协议机端到端联动 (证据 2): 真实 SOES ESC_foeprocess 经由已注册的 write_cb 自动驱动核心状态机处理 WRQ->DATA，覆盖成功 ACK 与校验失败 FOE Abort。
 *
 * 环境与限制说明:
 * - 本测试通过在宿主环境模拟 SOES 所需的最小邮箱结构 (_MBX, _MBXcontrol, ESCvar) 直接链接真实 esc_foe.c；
 * - 夹具统一采用静态存储，避免在调用栈帧上分配大对象；
 * - 明确未运行内容: 未在物理 ESC 硬件（如 LAN9252）或物理总线上运行，未包含真实 PHY/MII 交互。
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>

#include "esc.h"
#include "esc_foe.h"
#include "ecat_foe_efw.h"
#include "ecat_foe_core.h"
#include "ecat_foe_soes.h"
#include "mock_driver.h"

/* 模拟 SOES 协议栈所需的 5 个外部符号 */
_ESCvar ESCvar;
_MBXcontrol MBXcontrol[4];
uint8_t MBX[128 * 4];

uint8_t ESC_claimbuffer(void)
{
    return 1; /* 始终使用 mailbox 1 作为响应缓冲 */
}

/* 核心防回滚端口桩实现 */
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

/* 回调追踪与模拟变量 */
static int g_mock_write_calls = 0;
static size_t g_last_written_length = 0;
static uint32_t g_mock_write_return_code = 0;

static uint32_t mock_foe_write(foe_file_cfg_t *self, uint8_t *data, size_t len)
{
    (void)self;
    (void)data;
    g_mock_write_calls++;
    g_last_written_length = len;
    return g_mock_write_return_code;
}

/* 静态 FoE 配置文件表 */
static uint8_t s_foe_fbuffer[256];
static foe_file_cfg_t s_foe_files[2] = {
    {
        .name = "firmware_mcu.bin",
        .max_data = 4096,
        .dest_start_address = 0,
        .address_offset = 0,
        .total_size = 0,
        .filepass = 0,
        .write_only_in_boot = 0,
        .padding = 0,
        .write_function = mock_foe_write,
    },
    {
        .name = "firmware_fpga.bin",
        .max_data = 4096,
        .dest_start_address = 0,
        .address_offset = 0,
        .total_size = 0,
        .filepass = 0,
        .write_only_in_boot = 0,
        .padding = 0,
        .write_function = mock_foe_write,
    }
};

static foe_cfg_t s_foe_cfg = {
    .fbuffer = s_foe_fbuffer,
    .buffer_size = 256,
    .n_files = 2,
    .files = s_foe_files,
};

static void reset_soes_environment(void)
{
    memset(&ESCvar, 0, sizeof(ESCvar));
    memset(MBXcontrol, 0, sizeof(MBXcontrol));
    memset(MBX, 0, sizeof(MBX));

    ESCvar.MBXrun = 1;
    ESCvar.activembxsize = 128;
    ESCvar.ALstatus = ESCboot;

    g_mock_write_calls = 0;
    g_last_written_length = 0;
    g_mock_write_return_code = 0;

    FOE_config(&s_foe_cfg);
    FOE_init();
}

/* Test 1: WRQ 阶段首块回调时机核查 */
static void test_real_soes_wrq_callback_timing(void)
{
    printf("--- [Harness Test 1] 真实 SOES WRQ 首块回调时机核查 ---\n");
    reset_soes_environment();

    /* 注入 WRQ 报文至 MBX[0] */
    MBXcontrol[0].state = MBXstate_inclaim;
    _FOE *req = (_FOE *)&MBX[0];
    const char *fn = "firmware_mcu.bin";
    size_t fn_len = strlen(fn);

    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + fn_len);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    memcpy(req->filename, fn, fn_len);

    ESC_foeprocess();

    /* 证据: WRQ 处理后，write_function 从未被调用 */
    printf("  WRQ 处理后 write_function 调用次数: %d\n", g_mock_write_calls);
    assert(g_mock_write_calls == 0);

    /* 验证响应为 ACK(0) */
    _FOE *ack = (_FOE *)&MBX[1 * ESC_MBXSIZE];
    assert(ack->foeheader.opcode == FOE_OP_ACK);
    assert(ack->foeheader.packetnumber == 0);
    printf("  [PASS] 真实 SOES 在收到 WRQ 阶段不调用 write_function，并回复 ACK(0)\n");
}

/* Test 2: 文件名前缀匹配核查 */
static void test_real_soes_filename_prefix_matching(void)
{
    printf("--- [Harness Test 2] 真实 SOES 文件名前缀匹配核查 ---\n");
    reset_soes_environment();

    /* 子测试 2A: 下发短前缀 "firmware" (8 字节)，预期匹配 files[0] "firmware_mcu.bin" (16 字节) */
    MBXcontrol[0].state = MBXstate_inclaim;
    _FOE *req = (_FOE *)&MBX[0];
    const char *prefix = "firmware";
    size_t pfx_len = strlen(prefix);

    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + pfx_len);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    memcpy(req->filename, prefix, pfx_len);

    ESC_foeprocess();

    _FOE *ack = (_FOE *)&MBX[1 * ESC_MBXSIZE];
    assert(ack->foeheader.opcode == FOE_OP_ACK);
    printf("  [PASS] 下发前缀 '%s' (长度 %zu) 命中配置 'firmware_mcu.bin' 并返回 ACK (证明 strncmp 前缀匹配特性)\n", prefix, pfx_len);

    /* 子测试 2B: 下发完全不匹配文件名 "unknown.bin"，预期返回 FOE_ERR_NOTFOUND (0x8001) */
    reset_soes_environment();
    MBXcontrol[0].state = MBXstate_inclaim;
    req = (_FOE *)&MBX[0];
    const char *unknown_fn = "unknown.bin";
    size_t unk_len = strlen(unknown_fn);

    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + unk_len);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    memcpy(req->filename, unknown_fn, unk_len);

    ESC_foeprocess();

    _FOE *err_ack = (_FOE *)&MBX[1 * ESC_MBXSIZE];
    assert(err_ack->foeheader.opcode == FOE_OP_ERR);
    assert(err_ack->foeheader.errorcode == 0x8001);
    printf("  [PASS] 下发未配置名称 '%s' 正确返回 FOE_ERR_NOTFOUND (0x8001)\n", unknown_fn);
}

/* Test 3: 零长度包返回值被忽略 vs 短包返回值严格核验对比 */
static void test_real_soes_zero_len_vs_short_packet(void)
{
    printf("--- [Harness Test 3] 零长度包返回值忽略 vs 短包核验对比 ---\n");

    /* 子测试 3A: 短包分支 (数据长度 50 < 116)，write_function 返回失败 (1) */
    reset_soes_environment();

    /* 先通过 WRQ */
    MBXcontrol[0].state = MBXstate_inclaim;
    _FOE *req = (_FOE *)&MBX[0];
    memcpy(req->filename, "firmware_mcu.bin", 16);
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 16);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    ESC_foeprocess();

    /* 发送短数据包 50 字节，注入失败 */
    g_mock_write_return_code = 1; /* 模拟提交失败 */
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 50);
    req->foeheader.opcode = FOE_OP_DATA;
    req->foeheader.packetnumber = 1;
    memset(req->data, 0x11, 50);

    ESC_foeprocess();

    _FOE *res_mbx = (_FOE *)&MBX[1 * ESC_MBXSIZE];
    printf("  短包 (失败注入) SOES 响应: opcode=0x%04X, errorcode=0x%08X\n",
           res_mbx->foeheader.opcode, res_mbx->foeheader.errorcode);
    assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
    assert(res_mbx->foeheader.errorcode == 0x800B); /* FOE_ERR_PROGERROR */
    printf("  [PASS] 短包分支下 FOE_fclose() 返回非零时，真实 SOES 严格中断并返回 FOE_ERR_PROGERROR\n");

    /* 子测试 3B: 零长度结束包分支 (整包 116 字节满包后跟 0 长度包)，write_function 在 fclose 返回失败 (1) */
    reset_soes_environment();

    /* WRQ */
    MBXcontrol[0].state = MBXstate_inclaim;
    req = (_FOE *)&MBX[0];
    memcpy(req->filename, "firmware_mcu.bin", 16);
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 16);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    ESC_foeprocess();

    /* DATA 1: 满包 116 字节，正常接收 (不触发 flush，因 buffer_size=256) */
    g_mock_write_return_code = 0;
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + ESC_FOE_DATA_SIZE);
    req->foeheader.opcode = FOE_OP_DATA;
    req->foeheader.packetnumber = 1;
    memset(req->data, 0x22, ESC_FOE_DATA_SIZE);
    ESC_foeprocess();

    res_mbx = (_FOE *)&MBX[1 * ESC_MBXSIZE];
    assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
    assert(res_mbx->foeheader.packetnumber == 1);

    /* DATA 2: 0 长度包结束，注入回调失败 (1) */
    g_mock_write_return_code = 1; /* 模拟尾包写入/校验失败 */
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 0);
    req->foeheader.opcode = FOE_OP_DATA;
    req->foeheader.packetnumber = 2;

    ESC_foeprocess();

    res_mbx = (_FOE *)&MBX[1 * ESC_MBXSIZE];
    printf("  零长度包 (失败注入) SOES 响应: opcode=0x%04X, packet=%u\n",
           res_mbx->foeheader.opcode, res_mbx->foeheader.packetnumber);
    /* 关键证据: 真实 SOES 忽略了 FOE_fclose() 返回值，回复了 ACK(2) 而非 ERR(0x800B) */
    assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
    assert(res_mbx->foeheader.packetnumber == 2);
    printf("  [PASS] 零长度包分支下 FOE_fclose() 返回非零时，真实 SOES 果然忽略返回值并错误发送了成功 ACK\n");
}

/* 静态构造测试用 .efw 固件流 (64 字节头 + payload) */
static uint8_t s_efw_test_buf[256];

static size_t build_harness_test_efw(
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
    /* 0x00: magic "ECFW" */
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

/* 静态夹具驱动 */
static mock_driver_t s_harness_drv;

/* Test 4: 孤立直接调用防御核查 (非协议机驱动的直接接口单测) */
static void test_direct_api_calls_defense_without_wrq(void)
{
    printf("--- [Harness Test 4] 孤立直接调用防御核查 (非协议机驱动的直接接口单测) ---\n");
    reset_soes_environment();

    mock_driver_init(&s_harness_drv);

    ecat_foe_core_t core;
    memset(&core, 0, sizeof(core));

    ecat_foe_core_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.project_id = 0x10010001U;
    cfg.project_name = "SERVO_DRV";
    cfg.hardware_id = 0x00000001U;
    cfg.local_hw_ver = 1U;
    cfg.anti_rollback_enable = false;
    cfg.max_header_size = 128U;

    cfg.targets[0].target_id = 1;
    cfg.targets[0].enabled = true;
    cfg.targets[0].max_payload_size = 4096U;
    cfg.targets[0].write_unit = 8U;
    cfg.targets[0].driver = &s_harness_drv.driver;
    cfg.targets[0].user_ctx = &s_harness_drv;

    int ret_init = ecat_foe_core_init(&core, &cfg);
    assert(ret_init == 0);

    static uint8_t dummy_chunk[64];
    memset(dummy_chunk, 0xA5, sizeof(dummy_chunk));

    /* 证据类型 1: 直接单测调用 core_on_data (未经 WRQ 建立会话，直接调用被拒绝) */
    int st = ecat_foe_core_on_data(&core, dummy_chunk, sizeof(dummy_chunk));
    assert(st == -1);
    assert(core.last_error == ECAT_FOE_CORE_ERR_INVALID_STATE);
    assert(s_harness_drv.count_init == 0);

    /* 证据类型 1: 直接单测调用 adapter_write_cb (未经 WRQ 建立会话，直接回调被拒绝) */
    ecat_foe_soes_adapter_t adapter;
    ecat_foe_soes_init(&adapter, &core, 512);
    foe_file_cfg_t file_cfg;
    memset(&file_cfg, 0, sizeof(file_cfg));
    uint32_t cb_ret = ecat_foe_soes_write_cb(&file_cfg, dummy_chunk, sizeof(dummy_chunk));
    assert(cb_ret != 0);

    printf("  [PASS] 孤立直接调用防御: core_on_data 返回 -1 (last_error=0x%04X), write_cb 返回非零 (%u)\n",
           core.last_error, cb_ret);
}

/* Test 5: 真实 SOES ESC_foeprocess 驱动已注册适配器回调端到端联动核查 */
static void test_real_soes_driven_adapter_e2e_flow(void)
{
    printf("--- [Harness Test 5] 真实 SOES 驱动已注册适配器回调端到端联动核查 ---\n");

    /* 子测试 5A: 正常固件升级全流程 (WRQ -> DATA -> write_cb -> core_on_data -> finalize -> ACK) */
    reset_soes_environment();

    /* 绑定真实适配器回调至真实 SOES 文件表项 */
    s_foe_files[0].name = "SERVO_DRV_MCU_V1.0.0.0.efw";
    s_foe_files[0].write_function = ecat_foe_soes_write_cb;

    mock_driver_init(&s_harness_drv);

    ecat_foe_core_t core;
    memset(&core, 0, sizeof(core));

    ecat_foe_core_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.project_id = 0x10010001U;
    cfg.project_name = "SERVO_DRV";
    cfg.hardware_id = 0x00000001U;
    cfg.local_hw_ver = 1U;
    cfg.anti_rollback_enable = false;
    cfg.max_header_size = 128U;

    cfg.targets[0].target_id = 1;
    cfg.targets[0].enabled = true;
    cfg.targets[0].max_payload_size = 4096U;
    cfg.targets[0].write_unit = 8U;
    cfg.targets[0].driver = &s_harness_drv.driver;
    cfg.targets[0].user_ctx = &s_harness_drv;

    int ret_init = ecat_foe_core_init(&core, &cfg);
    assert(ret_init == 0);

    ecat_foe_soes_adapter_t adapter;
    ecat_foe_soes_init(&adapter, &core, s_foe_cfg.buffer_size);

    /* 1. 主站发送 WRQ 报文 */
    MBXcontrol[0].state = MBXstate_inclaim;
    _FOE *req = (_FOE *)&MBX[0];
    const char *fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
    size_t fn_len = strlen(fn);

    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + fn_len);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    memcpy(req->filename, fn, fn_len);

    /* 驱动真实 SOES 接收 WRQ */
    ESC_foeprocess();

    _FOE *res_mbx = (_FOE *)&MBX[1 * ESC_MBXSIZE];
    assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
    assert(res_mbx->foeheader.packetnumber == 0);

    /* 2. 应用层 WRQ 握手进入会话 */
    int wrq_ret = ecat_foe_soes_on_wrq(&adapter, fn, fn_len);
    assert(wrq_ret == 0);
    assert(core.state == ECAT_FOE_STATE_WRQ_ACKED);

    /* 3. 构造 16 字节 payload 的合法固件 */
    static const uint8_t s_payload[16] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
    };
    size_t efw_len = build_harness_test_efw(
        s_efw_test_buf,
        0x10010001U, /* project_id */
        0x00000001U, /* hardware_id */
        1, 1,        /* hw_ver_min, hw_ver_max */
        1,           /* target_id: MCU */
        0,           /* flags */
        0x01000000U, /* fw_version: 1.0.0.0 (Major=1 in MSB) */
        0,           /* min_allow_version */
        s_payload, 16);
    assert(efw_len == 80);

    /* 4. 主站发送 DATA 报文 (包号 1, 数据长度 80) */
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + efw_len);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_DATA;
    req->foeheader.packetnumber = 1;
    memcpy(req->data, s_efw_test_buf, efw_len);

    /* 驱动真实 SOES 处理 DATA 报文 -> 真实 SOES 自动调用已注册的 write_cb -> 核心状态机写入并 finalize */
    ESC_foeprocess();

    res_mbx = (_FOE *)&MBX[1 * ESC_MBXSIZE];
    assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
    assert(res_mbx->foeheader.packetnumber == 1);
    assert(s_harness_drv.count_init == 1);
    assert(s_harness_drv.count_write == 1);
    assert(s_harness_drv.count_finalize == 1);
    assert(core.state == ECAT_FOE_STATE_SUCCESS);
    printf("  [PASS] 真实 SOES 驱动 write_cb 成功流: WRQ->DATA 经真实协议栈驱动回调闭环，驱动 finalize 完成，SOES 回复 ACK(1)\n");

    /* 子测试 5B: 固件数据损坏触发真实 SOES FOE Abort 中止流 */
    reset_soes_environment();
    s_foe_files[0].name = "SERVO_DRV_MCU_V1.0.0.0.efw";
    s_foe_files[0].write_function = ecat_foe_soes_write_cb;

    mock_driver_init(&s_harness_drv);
    memset(&core, 0, sizeof(core));
    ret_init = ecat_foe_core_init(&core, &cfg);
    assert(ret_init == 0);
    ecat_foe_soes_init(&adapter, &core, s_foe_cfg.buffer_size);

    /* WRQ */
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + fn_len);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    memcpy(req->filename, fn, fn_len);
    ESC_foeprocess();
    wrq_ret = ecat_foe_soes_on_wrq(&adapter, fn, fn_len);
    assert(wrq_ret == 0);

    /* 故意破坏 header CRC (第 60 字节取反) */
    s_efw_test_buf[60] ^= 0xFF;

    /* 发送损坏的 DATA 报文 */
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + efw_len);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_DATA;
    req->foeheader.packetnumber = 1;
    memcpy(req->data, s_efw_test_buf, efw_len);

    /* 驱动真实 SOES: 内部调用 write_cb 失败 -> 真实 SOES 触发 FOE_abort(FOE_ERR_PROGERROR) */
    ESC_foeprocess();

    res_mbx = (_FOE *)&MBX[1 * ESC_MBXSIZE];
    assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
    assert(res_mbx->foeheader.errorcode == 0x800B);
    assert(s_harness_drv.count_finalize == 0);
    assert(core.state == ECAT_FOE_STATE_ERROR);
    printf("  [PASS] 真实 SOES 驱动 write_cb 失败流: 校验失败时核心机拒绝，真实 SOES 捕获 write_cb 失败并返回 FOE Abort 0x800B\n");
}

int main(void)
{
    printf("============================================================\n");
    printf(" 真实 SOES esc_foe 宿主独立验证 Harness\n");
    printf("============================================================\n");

    test_real_soes_wrq_callback_timing();
    test_real_soes_filename_prefix_matching();
    test_real_soes_zero_len_vs_short_packet();
    test_direct_api_calls_defense_without_wrq();
    test_real_soes_driven_adapter_e2e_flow();

    printf("------------------------------------------------------------\n");
    printf(">>> 真实 SOES esc_foe 5 项宿主行为验证全部通过！<<<\n");
    return 0;
}
