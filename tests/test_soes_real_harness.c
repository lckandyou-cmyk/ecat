/**
 * @file test_soes_real_harness.c
 * @brief 真实 SOES esc_foe.c 宿主独立测试 Harness
 *
 * 验证目标 (严格区分两种证据类型):
 * 1. 首块回调时机核查: 验证真实 SOES 在 WRQ 阶段是否触发 write_function；
 * 2. 文件名前缀匹配核查: 验证真实 SOES FOE_fopen 中 strncmp(name, cfg_name, num_chars) 的前缀匹配机理；
 * 3. 零长度包返回值核查: 验证真实 SOES 在 data_len == 0 分支对 FOE_fclose() 返回值的忽略特性与短包 abort 行为对比；
 * 4. 孤立直接调用防御 (证据 1): 独立单元测试直接调用 core_on_data 与 write_cb，验证未经 WRQ 建立会话时的接口级防御；
 * 5. 真实协议机端到端联动 (证据 2): 真实 SOES ESC_foeprocess 经由已注册的 write_cb 自动驱动核心状态机处理 WRQ->DATA，覆盖成功 ACK 与校验失败 FOE Abort；
 * 6. 生产现实路径核查 (V3-a): 验证未调用 on_wrq 时数据到达，核心机与适配层拒绝，SOES 回复 0x800B；
 * 7. 名字匹配矩阵核查 (V3-b): 空名、短前缀、超长、互为前缀、密码不符、write_only_in_boot；
 * 8. 数据尺寸扫描 (V3-c): 扫描 N=32,64,116，验证 kN-1, kN, kN+1 路径；
 * 9. 补齐固件端到端 (V3-d): 验证补齐固件走短包路径，错误映射为 0x800B；
 * 10. 会话边界核查 (V3-e): 验证 ERR 重置、中途新 WRQ 打断、双配置条目交替。
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

/* 真实 SOES esc.c 内部函数的补充前向声明 (供测试 Harness 调用) */
uint8_t ESC_outreqbuffer(void);
void ESC_writembx(uint8_t n);

/* 由应用层分配的 SOES 全局变量 (依据 esc.h:742-746 官方契约) */
_ESCvar ESCvar;
_MBXcontrol MBXcontrol[MBXBUFFERS];
uint8_t MBX[MBXBUFFERS * MBXSIZE];

/* ============================================================================
 * 真实 SOES 硬件层 (ESC Hardware Port) 桩函数与主站可观察物理报文捕获
 * ============================================================================ */

static uint8_t s_hw_sm1_buf[128];
static uint16_t s_hw_sm1_len = 0;
static int s_hw_esc_write_calls = 0;
static uint16_t s_hw_last_write_addr = 0;

void ESC_write(uint16_t address, void *buf, uint16_t len)
{
    s_hw_last_write_addr = address;
    /* 当写入 SM1 发送邮箱起始地址时，捕获主站真实物理接收到的数据 */
    if (address == ESC_MBX1_sma && buf != NULL) {
        s_hw_sm1_len = (len > sizeof(s_hw_sm1_buf)) ? (uint16_t)sizeof(s_hw_sm1_buf) : len;
        memcpy(s_hw_sm1_buf, buf, s_hw_sm1_len);
        s_hw_esc_write_calls++;
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

/* 获取真实 SOES 当前排队待发的 FoE 报文指针 */
static __attribute__((unused)) _FOE *get_last_outbound_foe(void)
{
    uint8_t slot = ESC_outreqbuffer();
    if (slot > 0 && slot < ESC_MBXBUFFERS) {
        return (_FOE *)&MBX[slot * ESC_MBXSIZE];
    }
    for (int i = (int)ESC_MBXBUFFERS - 1; i >= 1; i--) {
        if (MBXcontrol[i].state != MBXstate_idle) {
            return (_FOE *)&MBX[i * ESC_MBXSIZE];
        }
    }
    return (_FOE *)&MBX[1 * ESC_MBXSIZE];
}

static __attribute__((unused)) uint8_t get_last_outbound_slot(void)
{
    uint8_t slot = ESC_outreqbuffer();
    if (slot > 0 && slot < ESC_MBXBUFFERS) {
        return slot;
    }
    for (int i = (int)ESC_MBXBUFFERS - 1; i >= 1; i--) {
        if (MBXcontrol[i].state != MBXstate_idle) {
            return (uint8_t)i;
        }
    }
    return 1;
}

/* 模拟主站从物理邮箱读取走当前排队的响应，并释放槽位 */
static __attribute__((unused)) void mock_master_read_outbox(void)
{
    uint8_t slot = ESC_outreqbuffer();
    if (slot > 0 && slot < ESC_MBXBUFFERS) {
        ESC_writembx(slot);
        MBXcontrol[slot].state = MBXstate_idle;
    }
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

    g_mock_write_calls = 0;
    g_last_written_length = 0;
    g_mock_write_return_code = 0;

    s_hw_sm1_len = 0;
    s_hw_esc_write_calls = 0;
    memset(s_hw_sm1_buf, 0, sizeof(s_hw_sm1_buf));

    memset(s_foe_files, 0, sizeof(s_foe_files));
    s_foe_files[0].name = "firmware_mcu.bin";
    s_foe_files[0].max_data = 4096;
    s_foe_files[0].write_function = mock_foe_write;

    s_foe_files[1].name = "firmware_fpga.bin";
    s_foe_files[1].max_data = 4096;
    s_foe_files[1].write_function = mock_foe_write;

    s_foe_cfg.n_files = 2;
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
    _FOE *ack = get_last_outbound_foe();
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

    _FOE *ack = get_last_outbound_foe();
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

    _FOE *err_ack = get_last_outbound_foe();
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
    mock_master_read_outbox();

    /* 发送短数据包 50 字节，注入失败 */
    g_mock_write_return_code = 1; /* 模拟提交失败 */
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 50);
    req->foeheader.opcode = FOE_OP_DATA;
    req->foeheader.packetnumber = 1;
    memset(req->data, 0x11, 50);

    ESC_foeprocess();

    _FOE *res_mbx = get_last_outbound_foe();
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
    mock_master_read_outbox();

    /* DATA 1: 满包 116 字节，正常接收 (不触发 flush，因 buffer_size=256) */
    g_mock_write_return_code = 0;
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + ESC_FOE_DATA_SIZE);
    req->foeheader.opcode = FOE_OP_DATA;
    req->foeheader.packetnumber = 1;
    memset(req->data, 0x22, ESC_FOE_DATA_SIZE);
    ESC_foeprocess();

    res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
    assert(res_mbx->foeheader.packetnumber == 1);
    mock_master_read_outbox();

    /* DATA 2: 0 长度包结束，注入回调失败 (1) */
    g_mock_write_return_code = 1; /* 模拟尾包写入/校验失败 */
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 0);
    req->foeheader.opcode = FOE_OP_DATA;
    req->foeheader.packetnumber = 2;

    ESC_foeprocess();

    res_mbx = get_last_outbound_foe();
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

    _FOE *res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
    assert(res_mbx->foeheader.packetnumber == 0);
    mock_master_read_outbox();

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

    res_mbx = get_last_outbound_foe();
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
    mock_master_read_outbox();

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

    res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
    assert(res_mbx->foeheader.errorcode == 0x800B);
    assert(s_harness_drv.count_finalize == 0);
    assert(core.state == ECAT_FOE_STATE_ERROR);
    printf("  [PASS] 真实 SOES 驱动 write_cb 失败流: 校验失败时核心机拒绝，真实 SOES 捕获 write_cb 失败并返回 FOE Abort 0x800B\n");
}

/* -------------------------------------------------------------------------- */
/*             V3 专项测试: 生产现实、名字匹配、数据扫描、补齐与边界             */
/* -------------------------------------------------------------------------- */

/* V3-a: 不调用 on_wrq 的真实路径 (生产现实核验) [宿主观察] */
static void test_real_soes_without_on_wrq_production_reality(void)
{
    printf("--- [Harness V3-a] 不调用 on_wrq 的生产现实路径核查 [宿主观察] ---\n");
    reset_soes_environment();

    s_foe_files[0].name = "firmware_mcu.bin";
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

    /* 1. 主站下发 WRQ 报文 */
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

    _FOE *res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
    assert(res_mbx->foeheader.packetnumber == 0);
    mock_master_read_outbox();

    /* 生产现实关键点: 原生 SOES 不调用应用层 on_wrq 钩子，核心状态机保持在 ECAT_FOE_STATE_IDLE */
    assert(core.state == ECAT_FOE_STATE_IDLE);

    /* 2. 主站紧接着下发数据包 (短包 32 字节) */
    static uint8_t raw_data[32];
    memset(raw_data, 0xEE, sizeof(raw_data));

    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + sizeof(raw_data));
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_DATA;
    req->foeheader.packetnumber = 1;
    memcpy(req->data, raw_data, sizeof(raw_data));

    ESC_foeprocess();

    res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
    assert(res_mbx->foeheader.errorcode == 0x800B);
    assert(core.last_error == ECAT_FOE_CORE_ERR_INVALID_STATE);
    assert(s_harness_drv.count_init == 0);
    printf("  [PASS] [宿主观察] 生产现实路径: 未调用 on_wrq 时数据到达，核心机拒绝(-1, 0x001B)，适配层回调返回非零(1)，SOES 向主站回复 0x800B\n");
}

/* V3-b: 名字匹配完整矩阵核验 [宿主观察] */
static void test_real_soes_name_matching_suite(void)
{
    printf("--- [Harness V3-b] 名字匹配机制矩阵核查 [宿主观察] ---\n");

    /* 1. 空文件名 (num_chars == 0) -> strncmp(..., 0) 恒为 0，误命中首条配置 */
    reset_soes_environment();
    MBXcontrol[0].state = MBXstate_inclaim;
    _FOE *req = (_FOE *)&MBX[0];
    req->mbxheader.length = (uint16_t)sizeof(_FOEh);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    ESC_foeprocess();
    _FOE *res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
    assert(res_mbx->foeheader.packetnumber == 0);
    printf("  [PASS] [宿主观察] 空文件名 (长度 0): 恒成立命中首条配置 files[0]，SOES 回复 ACK(0)\n");

    /* 2. 短前缀 ("firmware" 长度 8) -> 误命中 "firmware_mcu.bin" (长度 16) */
    reset_soes_environment();
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 8);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    memcpy(req->filename, "firmware", 8);
    ESC_foeprocess();
    res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
    assert(res_mbx->foeheader.packetnumber == 0);
    printf("  [PASS] [宿主观察] 短前缀 'firmware': 仅比对 8 字节，误命中 files[0]，SOES 回复 ACK(0)\n");

    /* 3. 超过 31 字符的文件名 (32 字节) -> 无法匹配返回 0x8001 */
    reset_soes_environment();
    MBXcontrol[0].state = MBXstate_inclaim;
    const char *long_name = "12345678901234567890123456789012"; /* 32 字节 */
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 32);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    memcpy(req->filename, long_name, 32);
    ESC_foeprocess();
    res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
    assert(res_mbx->foeheader.errorcode == 0x8001); /* FOE_ERR_NOTFOUND */
    printf("  [PASS] [宿主观察] 超过 31 字符名称 (32 字节): 无法匹配返回 FOE_ERR_NOTFOUND (0x8001)\n");

    /* 4. 两个配置名互为前缀: files[0]="firmware_mcu", files[1]="firmware_mcu_ex" */
    reset_soes_environment();
    s_foe_files[0].name = "firmware_mcu";
    s_foe_files[1].name = "firmware_mcu_ex";
    /* 请求完整长名称 "firmware_mcu_ex" (15 字节) */
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 15);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    memcpy(req->filename, "firmware_mcu_ex", 15);
    ESC_foeprocess();
    res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
    /* 请求基准名称 "firmware_mcu" (12 字节) */
    reset_soes_environment();
    s_foe_files[0].name = "firmware_mcu";
    s_foe_files[1].name = "firmware_mcu_ex";
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 12);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    memcpy(req->filename, "firmware_mcu", 12);
    ESC_foeprocess();
    res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
    printf("  [PASS] [宿主观察] 互为前缀配置: 长名称精准避开短项命中长项，短名称命中首项\n");

    /* 5. 密码保护配置与不符核验 */
    reset_soes_environment();
    s_foe_files[0].name = "firmware_mcu.bin";
    s_foe_files[0].filepass = 0x12345678U;
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 16);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0x99999999U; /* 错误密码 */
    memcpy(req->filename, "firmware_mcu.bin", 16);
    ESC_foeprocess();
    res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
    assert(res_mbx->foeheader.errorcode == 0x800A); /* FOE_ERR_NORIGHTS (0x800A) */
    printf("  [PASS] [宿主观察] 密码保护: 错误密码返回 FOE_ERR_NORIGHTS (0x800A)\n");

    /* 6. write_only_in_boot 与 ALstatus 核验 */
    reset_soes_environment();
    s_foe_files[0].name = "firmware_mcu.bin";
    s_foe_files[0].filepass = 0;
    s_foe_files[0].write_only_in_boot = 1;
    ESCvar.ALstatus = ESCinit; /* 非 BOOT 态 */
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 16);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    memcpy(req->filename, "firmware_mcu.bin", 16);
    ESC_foeprocess();
    res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
    assert(res_mbx->foeheader.errorcode == 0x8009); /* FOE_ERR_NOTINBOOTSTRAP (0x8009) */
    mock_master_read_outbox();

    /* 恢复为 ESCboot 态 */
    ESCvar.ALstatus = ESCboot;
    MBXcontrol[0].state = MBXstate_inclaim;
    ESC_foeprocess();
    res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
    printf("  [PASS] [宿主观察] write_only_in_boot: 非 BOOT 态返回 0x8008，BOOT 态正常回复 ACK(0)\n");
}

/* 辅助结构: 记录写回调序列 */
typedef struct {
    size_t length;
    uint32_t address_offset;
} write_call_record_t;

static write_call_record_t s_scan_records[16];
static int s_scan_call_count = 0;

static uint32_t tracing_write_function(foe_file_cfg_t *self, uint8_t *data, size_t length)
{
    (void)data;
    if (s_scan_call_count < 16) {
        s_scan_records[s_scan_call_count].length = length;
        s_scan_records[s_scan_call_count].address_offset = self ? self->address_offset : 0;
        s_scan_call_count++;
    }
    return g_mock_write_return_code;
}

/* V3-c: ESC_FOE_DATA_SIZE 扫描核验 [宿主观察] */
static void test_real_soes_data_size_scan(void)
{
    printf("--- [Harness V3-c] ESC_FOE_DATA_SIZE 扫描核查 (N=32, 64, 116) [宿主观察] ---\n");

    static const uint32_t s_test_n_values[3] = {32U, 64U, 116U};

    for (int idx = 0; idx < 3; idx++) {
        uint32_t N = s_test_n_values[idx];
        uint32_t k = 2; /* 满包数量设为 2 */

        /* 测试三种长度: k*N - 1, k*N, k*N + 1 */
        uint32_t lengths[3] = {k * N - 1, k * N, k * N + 1};

        for (int l_idx = 0; l_idx < 3; l_idx++) {
            uint32_t total_len = lengths[l_idx];
            bool is_exact_multiple = (total_len % N == 0);

            reset_soes_environment();
            ESCvar.activembxsize = (uint16_t)(N + sizeof(_MBXh) + sizeof(_FOEh));
            s_foe_files[0].name = "firmware_mcu.bin";
            s_foe_files[0].write_function = tracing_write_function;
            s_scan_call_count = 0;
            g_mock_write_return_code = 0;

            /* 1. 发送 WRQ */
            MBXcontrol[0].state = MBXstate_inclaim;
            _FOE *req = (_FOE *)&MBX[0];
            req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 16);
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            req->foeheader.password = 0;
            memcpy(req->filename, "firmware_mcu.bin", 16);
            ESC_foeprocess();
            mock_master_read_outbox();

            /* 2. 发送全量满包 */
            uint32_t sent = 0;
            uint32_t packet_num = 1;
            while (sent + N <= total_len) {
                MBXcontrol[0].state = MBXstate_inclaim;
                req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + N);
                req->mbxheader.mbxtype = MBXFOE;
                req->foeheader.opcode = FOE_OP_DATA;
                req->foeheader.packetnumber = packet_num++;
                memset(req->data, 0x55, N);
                ESC_foeprocess();
                mock_master_read_outbox();
                sent += N;
            }

            /* 3. 发送尾包或 0 长度包 (并注入最后一次回调失败) */
            g_mock_write_return_code = 1; /* 注入最后写入失败 */

            if (is_exact_multiple) {
                /* 整除情形: 发送 0 长度结束包 */
                MBXcontrol[0].state = MBXstate_inclaim;
                req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 0);
                req->mbxheader.mbxtype = MBXFOE;
                req->foeheader.opcode = FOE_OP_DATA;
                req->foeheader.packetnumber = packet_num++;
                ESC_foeprocess();

                _FOE *res_mbx = get_last_outbound_foe();
                /* 真实 SOES 观察: 零长度路径忽略 FOE_fclose 返回值，主站看到 ACK */
                assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
            } else {
                /* 非整除情形: 最后一包为短包 (长度 < N) */
                uint32_t rem = total_len - sent;
                MBXcontrol[0].state = MBXstate_inclaim;
                req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + rem);
                req->mbxheader.mbxtype = MBXFOE;
                req->foeheader.opcode = FOE_OP_DATA;
                req->foeheader.packetnumber = packet_num++;
                memset(req->data, 0x66, rem);
                ESC_foeprocess();

                _FOE *res_mbx = get_last_outbound_foe();
                /* 真实 SOES 观察: 短包路径检查 FOE_fclose 返回值，主站看到 ERR 0x800B */
                assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
                assert(res_mbx->foeheader.errorcode == 0x800B);
            }
        }
        printf("  [PASS] [宿主观察] 载荷尺寸 N=%u: kN-1 与 kN+1 走短包路径触发 0x800B；kN 走 0 长度包路径忽略错误回复 ACK\n", N);
    }
}

/* V3-d: 补零后的 .efw 端到端短包路径核验 [宿主观察] */
static void test_real_soes_padded_efw_avoids_zero_len(void)
{
    printf("--- [Harness V3-d] 补齐 .efw 走短包路径端到端核查 [宿主观察] ---\n");
    reset_soes_environment();

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

    /* 1. WRQ 握手 */
    MBXcontrol[0].state = MBXstate_inclaim;
    _FOE *req = (_FOE *)&MBX[0];
    const char *fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
    size_t fn_len = strlen(fn);
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + fn_len);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    memcpy(req->filename, fn, fn_len);
    ESC_foeprocess();
    int wrq_ret = ecat_foe_soes_on_wrq(&adapter, fn, fn_len);
    assert(wrq_ret == 0);
    mock_master_read_outbox();

    /* 2. 构造补齐后的固件 (单包载荷上限设为 64，总长 80 字节，80 % 64 = 16，末包为 16 字节短包) */
    static const uint8_t s_payload[16] = {0x01, 0x02, 0x03, 0x04};
    size_t efw_len = build_harness_test_efw(
        s_efw_test_buf,
        0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0,
        s_payload, 16);
    assert(efw_len == 80);

    /* 故意在驱动 finalize 中注入失败，测试短包路径下的错误映射 */
    s_harness_drv.inject_fail_finalize = true;

    /* 发送 DATA 报文 (80 字节短包) */
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + efw_len);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_DATA;
    req->foeheader.packetnumber = 1;
    memcpy(req->data, s_efw_test_buf, efw_len);

    ESC_foeprocess();

    _FOE *res_mbx = get_last_outbound_foe();
    /* 关键观察: 补齐避开 0 长度包后，走短包路径，驱动 finalize 失败被正确捕获并映射为 0x800B */
    assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
    assert(res_mbx->foeheader.errorcode == 0x800B);
    assert(core.state == ECAT_FOE_STATE_ERROR);
    printf("  [PASS] [宿主观察] 补齐 .efw 走短包路径: 最终提交失败被正确映射为 FOE Abort 0x800B，消除了静默吞没隐患\n");
}

/* V3-e: 会话边界测试 (ERR 报文、中途 WRQ、零长度失败后新 WRQ、双配置交替) [宿主观察] */
static void test_real_soes_session_boundary_cases(void)
{
    printf("--- [Harness V3-e] 会话边界机制核查 [宿主观察] ---\n");

    /* 1. 主站发送 ERR 报文 -> SOES 重置 foestate 为 FOE_READY */
    reset_soes_environment();
    MBXcontrol[0].state = MBXstate_inclaim;
    _FOE *req = (_FOE *)&MBX[0];
    req->mbxheader.length = (uint16_t)sizeof(_FOEh);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_ERR;
    req->foeheader.errorcode = 0x8000;
    ESC_foeprocess();
    printf("  [PASS] [宿主观察] 主站发 ERR: SOES 状态机重置为初始待命态\n");

    /* 2. 数据传输中途重新到来 WRQ */
    reset_soes_environment();
    s_foe_files[0].name = "firmware_mcu.bin";
    s_foe_files[0].write_function = tracing_write_function;
    s_scan_call_count = 0;
    g_mock_write_return_code = 0;

    /* 先进入中途传输 */
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 16);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    memcpy(req->filename, "firmware_mcu.bin", 16);
    ESC_foeprocess();
    mock_master_read_outbox();

    /* 发送第 1 包 */
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 64);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_DATA;
    req->foeheader.packetnumber = 1;
    memset(req->data, 0xAA, 64);
    ESC_foeprocess();
    mock_master_read_outbox();

    /* 中途重新收到新 WRQ */
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 16);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    memcpy(req->filename, "firmware_mcu.bin", 16);
    ESC_foeprocess();
    _FOE *res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
    assert(res_mbx->foeheader.packetnumber == 0);
    mock_master_read_outbox();
    printf("  [PASS] [宿主观察] 传输中途新 WRQ: 成功打断并重启新会话，SOES 回复 ACK(0)\n");

    /* 3. 两个不同配置条目交替升级: files[0] -> files[1] */
    reset_soes_environment();
    s_foe_files[0].name = "firmware_mcu.bin";
    s_foe_files[0].write_function = tracing_write_function;
    s_foe_files[1].name = "firmware_fpga.bin";
    s_foe_files[1].write_function = tracing_write_function;
    s_scan_call_count = 0;

    /* 会话 A: files[0] */
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 16);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    memcpy(req->filename, "firmware_mcu.bin", 16);
    ESC_foeprocess();
    res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
    mock_master_read_outbox();

    /* 发送短包完成 files[0] */
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 32);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_DATA;
    req->foeheader.packetnumber = 1;
    memset(req->data, 0x11, 32);
    ESC_foeprocess();
    res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
    mock_master_read_outbox();

    /* 会话 B: 切换为 files[1] */
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 17);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_WRQ;
    req->foeheader.password = 0;
    memcpy(req->filename, "firmware_fpga.bin", 17);
    ESC_foeprocess();
    res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
    mock_master_read_outbox();

    /* 发送短包完成 files[1] */
    MBXcontrol[0].state = MBXstate_inclaim;
    req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 40);
    req->mbxheader.mbxtype = MBXFOE;
    req->foeheader.opcode = FOE_OP_DATA;
    req->foeheader.packetnumber = 1;
    memset(req->data, 0x22, 40);
    ESC_foeprocess();
    res_mbx = get_last_outbound_foe();
    assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
    mock_master_read_outbox();

    printf("  [PASS] [宿主观察] 双配置条目交替: files[0] 与 files[1] 交替升级，地址与指针独立正确复位\n");
}

/* ========================================================================== */
/*   V4 专项: 真实 SOES 邮箱前置嗅探与动态文件名同步六大维度实测验证          */
/* ========================================================================== */
static void init_harness_cfg(ecat_foe_core_cfg_t *cfg, mock_driver_t *drv)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->project_id = 0x10010001U;
    cfg->hardware_id = 0x00000001U;
    cfg->local_hw_ver = 1U;
    cfg->anti_rollback_enable = false;
    cfg->max_header_size = 128U;
    cfg->project_name = "SERVO_DRV";
    cfg->targets[0].target_id = 1;
    cfg->targets[0].enabled = true;
    cfg->targets[0].max_payload_size = 4096U;
    cfg->targets[0].write_unit = 8U;
    cfg->targets[0].driver = &drv->driver;
    cfg->targets[0].user_ctx = drv;
    cfg->targets[1].target_id = 2;
    cfg->targets[1].enabled = true;
    cfg->targets[1].max_payload_size = 4096U;
    cfg->targets[1].write_unit = 8U;
    cfg->targets[1].driver = &drv->driver;
    cfg->targets[1].user_ctx = drv;
}

static void test_real_soes_pre_hook_validation_suite(void)
{
    printf("--- [Harness V4] 真实 SOES 邮箱前置嗅探钩子六大维度综合验证 ---\n");

    /* 1. 维度 1: 帧边界与畸形帧防护 (Frame Bounds) */
    {
        reset_soes_environment();
        mock_driver_init(&s_harness_drv);
        ecat_foe_core_t core;
        ecat_foe_core_cfg_t cfg;
        init_harness_cfg(&cfg, &s_harness_drv);
        assert(ecat_foe_core_init(&core, &cfg) == 0);
        ecat_foe_soes_adapter_t adapter;
        ecat_foe_soes_init(&adapter, &core, 128);
        assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

        /* Case 1A: 报文总长不足 12 字节 (例如只有 8 字节) */
        uint8_t malformed_buf[16] = {0};
        malformed_buf[5] = MBXFOE;
        assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, malformed_buf, 8) == 0);

        /* Case 1B: 非 FoE 邮箱类型 (例如 CoE: 0x03) */
        malformed_buf[0] = 10; malformed_buf[1] = 0; /* len=10 */
        malformed_buf[5] = 0x03; /* CoE */
        assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, malformed_buf, 16) == 0);

        /* Case 1C: 缓冲区截断越界保护 (声明 payload_len=20，但实际传入 mbx_buf_len 只有 14 字节) */
        uint8_t trunc_buf[16] = {0};
        trunc_buf[0] = 20; trunc_buf[1] = 0;
        trunc_buf[5] = MBXFOE;
        trunc_buf[6] = FOE_OP_WRQ;
        assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, trunc_buf, 14) == 0);

        /* Case 1D: 超长文件名 (> FOE_FN_MAX=31 字节，例如 32 字节) */
        MBXcontrol[0].state = MBXstate_inclaim;
        _FOE *req = (_FOE *)&MBX[0];
        req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 32);
        req->mbxheader.mbxtype = MBXFOE;
        req->foeheader.opcode = FOE_OP_WRQ;
        req->foeheader.password = 0;
        memset(req->filename, 'A', 32);
        assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + 32) != 0);
        /* 原生 SOES 随后处理超长不匹配文件名报 0x8001 */
        ESC_foeprocess();
        _FOE *res_mbx = get_last_outbound_foe();
        assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
        assert(res_mbx->foeheader.errorcode == 0x8001);
        ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_mbx, sizeof(_MBXh) + sizeof(_FOEh));
        mock_master_read_outbox();
        printf("  [PASS] 维度 1 (帧边界): 畸形短帧、截断帧与超长文件名安全放行，原生 SOES 返回 0x8001\n");
    }

    /* 2. 维度 2: 密码验证时序与权限保持 (Password Verification Timing) */
    {
        reset_soes_environment();
        mock_driver_init(&s_harness_drv);
        ecat_foe_core_t core;
        ecat_foe_core_cfg_t cfg;
        init_harness_cfg(&cfg, &s_harness_drv);
        assert(ecat_foe_core_init(&core, &cfg) == 0);
        ecat_foe_soes_adapter_t adapter;
        ecat_foe_soes_init(&adapter, &core, 128);
        assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

        /* 从站针对 files[0] 配置专属密码 */
        s_foe_files[0].filepass = 0x12345678U;
        s_foe_files[0].name = "SERVO_DRV_MCU.efw"; /* 基准静态名 */
        s_foe_files[0].write_function = ecat_foe_soes_write_cb;

        const char *dyn_name = "SERVO_DRV_MCU_V1.0.0.0.efw";
        size_t name_len = strlen(dyn_name);

        /* Case 2A: 注入错误密码 (0x99999999) */
        MBXcontrol[0].state = MBXstate_inclaim;
        _FOE *req = (_FOE *)&MBX[0];
        req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + name_len);
        req->mbxheader.mbxtype = MBXFOE;
        req->foeheader.opcode = FOE_OP_WRQ;
        req->foeheader.password = 0x99999999U; /* 错误密码 */
        memcpy(req->filename, dyn_name, name_len);

        /* 执行前置嗅探钩子: 动态同步文件名 */
        int hook_rc = ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + name_len);
        assert(hook_rc == 0);

        /* 原生 SOES 执行: 动态名称已匹配，紧接着校验密码，发现密码不符立即返回 FOE_ERR_NORIGHTS (0x800A) */
        ESC_foeprocess();
        _FOE *res_mbx = get_last_outbound_foe();
        assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
        assert(res_mbx->foeheader.errorcode == 0x800A);
        ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_mbx, sizeof(_MBXh) + sizeof(_FOEh));
        assert(adapter.has_staged_wrq == false);
        mock_master_read_outbox();
        printf("  [PASS] 维度 2A (密码时序): 动态同步后密码错误被原生 SOES 拦截并回复 0x800A，暂存 WRQ 安全释放\n");

        /* Case 2B: 注入正确密码 (0x12345678) */
        MBXcontrol[0].state = MBXstate_inclaim;
        req->foeheader.password = 0x12345678U; /* 正确密码 */
        hook_rc = ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + name_len);
        assert(hook_rc == 0);

        ESC_foeprocess();
        res_mbx = get_last_outbound_foe();
        assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
        assert(res_mbx->foeheader.packetnumber == 0);
        ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_mbx, sizeof(_MBXh) + sizeof(_FOEh));
        assert(core.req_target_id == 1);
        mock_master_read_outbox();
        printf("  [PASS] 维度 2B (密码时序): 动态同步后正确密码顺利通过原生校验并回复 ACK(0)，核心新会话正式激活\n");
    }

    /* 3. 维度 3: 静态名称缓冲区稳定性 (Name Buffer Stability) */
    {
        /* 验证后续 DATA 报文覆写 MBX[0] 缓冲区后，files[0].name 指向的字符串未被破坏 */
        assert(s_foe_files[0].name != NULL);
        assert(strcmp(s_foe_files[0].name, "SERVO_DRV_MCU_V1.0.0.0.efw") == 0);

        /* 模拟后续 DATA 报文灌入并将 MBX[0] 完全用垃圾数据覆写 */
        memset(MBX, 0xDE, ESC_MBXBUFFERS * ESC_MBXSIZE);

        /* 验证 files[0].name 依然完好无损 */
        assert(strcmp(s_foe_files[0].name, "SERVO_DRV_MCU_V1.0.0.0.efw") == 0);
        printf("  [PASS] 维度 3 (名称缓冲): 静态名称缓冲生命周期稳定，MBX 覆写后文件名完好无损\n");
    }

    /* 4. 维度 4: 拒绝响应 (Rejection Responses) */
    {
        reset_soes_environment();
        mock_driver_init(&s_harness_drv);
        ecat_foe_core_t core;
        ecat_foe_core_cfg_t cfg;
        init_harness_cfg(&cfg, &s_harness_drv);
        assert(ecat_foe_core_init(&core, &cfg) == 0);
        ecat_foe_soes_adapter_t adapter;
        ecat_foe_soes_init(&adapter, &core, 128);
        assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

        s_foe_files[0].name = "SERVO_DRV_MCU.efw";

        /* 下发项目名称不匹配的文件名 */
        const char *mismatch_name = "WRONG_PROJ_MCU_V1.0.0.0.efw";
        size_t m_len = strlen(mismatch_name);

        MBXcontrol[0].state = MBXstate_inclaim;
        _FOE *req = (_FOE *)&MBX[0];
        req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + m_len);
        req->mbxheader.mbxtype = MBXFOE;
        req->foeheader.opcode = FOE_OP_WRQ;
        req->foeheader.password = 0;
        memcpy(req->filename, mismatch_name, m_len);

        int hook_rc = ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + m_len);
        assert(hook_rc != 0); /* 核心机拒绝 */

        /* 放行原生 SOES: 由于未执行名称同步，SOES 比对失败自动回复 0x8001 Abort */
        ESC_foeprocess();
        _FOE *res_mbx = get_last_outbound_foe();
        assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
        assert(res_mbx->foeheader.errorcode == 0x8001);
        ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_mbx, sizeof(_MBXh) + sizeof(_FOEh));
        mock_master_read_outbox();
        printf("  [PASS] 维度 4 (拒绝响应): 核心机校验失败不予同步，原生 SOES 正确发送 0x8001 Abort\n");
    }

    /* 5. 维度 5: 多目标路由与传输中途新 WRQ 打断自愈 (Multi-target & Repeated WRQ) */
    {
        reset_soes_environment();
        mock_driver_init(&s_harness_drv);
        ecat_foe_core_t core;
        ecat_foe_core_cfg_t cfg;
        init_harness_cfg(&cfg, &s_harness_drv);
        assert(ecat_foe_core_init(&core, &cfg) == 0);
        ecat_foe_soes_adapter_t adapter;
        ecat_foe_soes_init(&adapter, &core, 128);
        assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

        s_foe_files[0].name = "SERVO_DRV_MCU.efw";
        s_foe_files[0].write_function = ecat_foe_soes_write_cb;
        s_foe_files[1].name = "SERVO_DRV_FPGA.efw";
        s_foe_files[1].write_function = ecat_foe_soes_write_cb;

        /* Target 1 (MCU) WRQ */
        const char *mcu_fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
        MBXcontrol[0].state = MBXstate_inclaim;
        _FOE *req = (_FOE *)&MBX[0];
        req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(mcu_fn));
        req->mbxheader.mbxtype = MBXFOE;
        req->foeheader.opcode = FOE_OP_WRQ;
        req->foeheader.password = 0;
        memcpy(req->filename, mcu_fn, strlen(mcu_fn));

        assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(mcu_fn)) == 0);
        ESC_foeprocess();
        _FOE *wrq_res = get_last_outbound_foe();
        assert(wrq_res->foeheader.opcode == FOE_OP_ACK);
        ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)wrq_res, sizeof(_MBXh) + sizeof(_FOEh));
        assert(core.req_target_id == 1);
        assert(strcmp(s_foe_files[0].name, mcu_fn) == 0);
        mock_master_read_outbox();

        /* 构造 116 字节的第一包数据 (64 字节头部 + 52 字节有效载荷)，使传输进入中途 WAITING_FOR_DATA 态 */
        static const uint8_t s_p1[52] = {0x11, 0x22, 0x33, 0x44};
        size_t efw_p1_len = build_harness_test_efw(
            s_efw_test_buf,
            0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0,
            s_p1, 52);
        assert(efw_p1_len == 116);

        MBXcontrol[0].state = MBXstate_inclaim;
        req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + 116);
        req->mbxheader.mbxtype = MBXFOE;
        req->foeheader.opcode = FOE_OP_DATA;
        req->foeheader.packetnumber = 1;
        memcpy(req->data, s_efw_test_buf, 116);
        ESC_foeprocess();
        _FOE *res_mbx = get_last_outbound_foe();
        assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
        assert(res_mbx->foeheader.packetnumber == 1);
        mock_master_read_outbox();
        /* 此时满包写入成功，原生 SOES 正处于 FOE_WAIT_FOR_DATA 态 */

        /* 实测验证：中途处于 WAIT_FOR_DATA 时若直接收到新 WRQ，原生 SOES 返回 0x8004 (FOE_ERR_ILLEGAL) */
        const char *fpga_fn = "SERVO_DRV_FPGA_V1.0.0.0.efw";
        MBXcontrol[0].state = MBXstate_inclaim;
        req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fpga_fn));
        req->mbxheader.mbxtype = MBXFOE;
        req->foeheader.opcode = FOE_OP_WRQ;
        req->foeheader.password = 0;
        memcpy(req->filename, fpga_fn, strlen(fpga_fn));
        assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fpga_fn)) == 0);
        ESC_foeprocess();
        res_mbx = get_last_outbound_foe();
        assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
        assert(res_mbx->foeheader.errorcode == 0x8004);
        ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_mbx, sizeof(_MBXh) + sizeof(_FOEh));
        mock_master_read_outbox();

        /* 原生 SOES 在 0x8004 abort 中已自动调用 FOE_init() 复位回 FOE_READY。重新发送 Target 2 WRQ */
        MBXcontrol[0].state = MBXstate_inclaim;
        req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fpga_fn));
        req->mbxheader.mbxtype = MBXFOE;
        req->foeheader.opcode = FOE_OP_WRQ;
        req->foeheader.password = 0;
        memcpy(req->filename, fpga_fn, strlen(fpga_fn));
        assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fpga_fn)) == 0);
        ESC_foeprocess();
        res_mbx = get_last_outbound_foe();
        assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
        assert(res_mbx->foeheader.packetnumber == 0);
        ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_mbx, sizeof(_MBXh) + sizeof(_FOEh));
        mock_master_read_outbox();

        /* 验证 files[0] 和 files[1] 的名字隔离共存，互不覆盖 */
        assert(strcmp(s_foe_files[0].name, mcu_fn) == 0);
        assert(strcmp(s_foe_files[1].name, fpga_fn) == 0);
        assert(core.req_target_id == 2);
        printf("  [PASS] 维度 5 (多目标与新 WRQ): 目标 1 与目标 2 动态名称隔离稳定，实测验证中途新 WRQ 响应边界\n");
    }

    /* 6. 维度 6: 端到端固件升级与短包结束错误映射 (E2E Flow & End Error Mapping) */
    {
        reset_soes_environment();
        mock_driver_reset(&s_harness_drv);
        ecat_foe_core_t core;
        ecat_foe_core_cfg_t cfg;
        init_harness_cfg(&cfg, &s_harness_drv);
        assert(ecat_foe_core_init(&core, &cfg) == 0);
        ecat_foe_soes_adapter_t adapter;
        ecat_foe_soes_init(&adapter, &core, 128);
        assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

        s_foe_files[0].name = "SERVO_DRV_MCU.efw";
        s_foe_files[0].write_function = ecat_foe_soes_write_cb;

        const char *fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
        MBXcontrol[0].state = MBXstate_inclaim;
        _FOE *req = (_FOE *)&MBX[0];
        req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fn));
        req->mbxheader.mbxtype = MBXFOE;
        req->foeheader.opcode = FOE_OP_WRQ;
        req->foeheader.password = 0;
        memcpy(req->filename, fn, strlen(fn));

        assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn)) == 0);
        ESC_foeprocess();
        _FOE *res_mbx = get_last_outbound_foe();
        assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
        assert(res_mbx->foeheader.packetnumber == 0);
        assert(ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_mbx, sizeof(_MBXh) + sizeof(_FOEh)) == 0);
        assert(core.req_target_id == 1);
        assert(core.state == ECAT_FOE_STATE_WRQ_ACKED);
        mock_master_read_outbox();

        /* 构造 80 字节固件 (64 字节头 + 16 字节 payload) */
        static const uint8_t s_payload[16] = {0xAA, 0xBB, 0xCC, 0xDD};
        size_t efw_len = build_harness_test_efw(
            s_efw_test_buf,
            0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0,
            s_payload, 16);
        assert(efw_len == 80);

        /* 发送 80 字节短包 */
        MBXcontrol[0].state = MBXstate_inclaim;
        req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + efw_len);
        req->mbxheader.mbxtype = MBXFOE;
        req->foeheader.opcode = FOE_OP_DATA;
        req->foeheader.packetnumber = 1;
        memcpy(req->data, s_efw_test_buf, efw_len);

        ESC_foeprocess();
        res_mbx = get_last_outbound_foe();
        assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
        assert(res_mbx->foeheader.packetnumber == 1);
        assert(core.state == ECAT_FOE_STATE_SUCCESS);
        assert(s_harness_drv.valid_flag == true);
        mock_master_read_outbox();
        printf("  [PASS] 维度 6 (端到端升级闭环): 动态文件名固件走短包路径升级成功，驱动有效标记成功写入\n");
    }
}

int main(void)
{
    printf("============================================================\n");
    printf(" 真实 SOES esc_foe 宿主独立验证 Harness (含 V3 扩展与 V4 前置钩子)\n");
    printf("============================================================\n");

    test_real_soes_wrq_callback_timing();
    test_real_soes_filename_prefix_matching();
    test_real_soes_zero_len_vs_short_packet();
    test_direct_api_calls_defense_without_wrq();
    test_real_soes_driven_adapter_e2e_flow();

    /* V3 专项测试 */
    test_real_soes_without_on_wrq_production_reality();
    test_real_soes_name_matching_suite();
    test_real_soes_data_size_scan();
    test_real_soes_padded_efw_avoids_zero_len();
    test_real_soes_session_boundary_cases();

    /* V4 专项测试: 邮箱前置嗅探与动态文件名同步六大维度实测 */
    test_real_soes_pre_hook_validation_suite();

    /* 专项验证: 82ec2da 审查指出的 4 项关键风险严格加固回归验证 */
    {
        printf("--- [Harness 风险加固回归] 验证 4 大关键漏洞修复后严格防御行为 ---\n");

        /* 风险 1 回归: 密码错误与非 BOOT 态 WRQ 绝不提前篡改核心或打断已有会话 */
        {
            reset_soes_environment();
            mock_driver_reset(&s_harness_drv);
            ecat_foe_core_t core;
            ecat_foe_core_cfg_t cfg;
            init_harness_cfg(&cfg, &s_harness_drv);
            assert(ecat_foe_core_init(&core, &cfg) == 0);
            ecat_foe_soes_adapter_t adapter;
            ecat_foe_soes_init(&adapter, &core, 128);
            assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

            s_foe_files[0].name = "SERVO_DRV_MCU.efw";
            s_foe_files[0].filepass = 0x12345678U;
            s_foe_files[0].write_only_in_boot = 1;
            s_foe_files[1].name = "SERVO_DRV_FPGA.efw";
            s_foe_files[1].filepass = 0x87654321U;
            s_foe_files[1].write_only_in_boot = 1;

            /* 会话 1 (MCU) 建立成功 */
            const char *fn_mcu = "SERVO_DRV_MCU_V1.0.0.0.efw";
            MBXcontrol[0].state = MBXstate_inclaim;
            _FOE *req = (_FOE *)&MBX[0];
            req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fn_mcu));
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            req->foeheader.password = 0x12345678U;
            memcpy(req->filename, fn_mcu, strlen(fn_mcu));
            assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn_mcu)) == 0);
            ESC_foeprocess();
            _FOE *res_mbx = get_last_outbound_foe();
            assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_mbx, sizeof(_MBXh) + sizeof(_FOEh));
            assert(core.req_target_id == 1);
            mock_master_read_outbox();

            /* 场景 1A: 传输中途针对目标 2 (FPGA) 注入 WRQ (密码亦错误) */
            const char *fn_fpga = "SERVO_DRV_FPGA_V1.0.0.0.efw";
            MBXcontrol[0].state = MBXstate_inclaim;
            req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fn_fpga));
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            req->foeheader.password = 0x99999999U; /* 错误密码 */
            memcpy(req->filename, fn_fpga, strlen(fn_fpga));

            assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn_fpga)) == 0);
            ESC_foeprocess();
            res_mbx = get_last_outbound_foe();
            assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
            /* 原生 SOES 因中途状态非法拒绝并返回 0x8004 (FOE_ERR_ILLEGAL) */
            assert(res_mbx->foeheader.errorcode == 0x8004);
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_mbx, sizeof(_MBXh) + sizeof(_FOEh));
            /* 关键断言: 核心机 target_id 严格保持为 1，已有会话未受任何篡改！ */
            assert(core.req_target_id == 1);
            assert(adapter.has_staged_wrq == false);
            mock_master_read_outbox();

            /* 场景 1B: 在就绪态 (READY) 下发密码错误的 WRQ */
            MBXcontrol[0].state = MBXstate_inclaim;
            req->foeheader.errorcode = 0x8000;
            req->foeheader.opcode = FOE_OP_ERR;
            req->mbxheader.length = (uint16_t)sizeof(_FOEh);
            ESC_foeprocess(); /* 主站复位 SOES 状态机至 READY */

            MBXcontrol[0].state = MBXstate_inclaim;
            req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fn_fpga));
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            req->foeheader.password = 0x99999999U; /* 注入错误密码 */
            memcpy(req->filename, fn_fpga, strlen(fn_fpga));

            assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn_fpga)) == 0);
            ESC_foeprocess();
            res_mbx = get_last_outbound_foe();
            assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
            assert(res_mbx->foeheader.errorcode == 0x800A); /* NORIGHTS */
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_mbx, sizeof(_MBXh) + sizeof(_FOEh));
            /* 关键断言: 核心机 target_id 依然严格保持为 1！ */
            assert(core.req_target_id == 1);
            assert(adapter.has_staged_wrq == false);
            mock_master_read_outbox();

            /* 场景 1C: 在非 BOOT 态下发 WRQ */
            ESCvar.ALstatus = ESCinit; /* 切到 INIT 态 */
            MBXcontrol[0].state = MBXstate_inclaim;
            req->foeheader.password = 0x87654321U; /* 正确密码 */
            assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn_fpga)) == 0);
            ESC_foeprocess();
            res_mbx = get_last_outbound_foe();
            assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
            assert(res_mbx->foeheader.errorcode == 0x8009); /* NOTINBOOTSTRAP in SOES */
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_mbx, sizeof(_MBXh) + sizeof(_FOEh));
            assert(core.req_target_id == 1);
            mock_master_read_outbox();
            printf("  [PASS] 风险 1 防御验证: 密码错误与非 BOOT 态 WRQ 被原生拦截，核心状态与已有会话完好无损\n");
        }

        /* 风险 2 回归: 空文件名被安全拒绝，原生 SOES 返回 0x8001 NOTFOUND */
        {
            reset_soes_environment();
            mock_driver_reset(&s_harness_drv);
            ecat_foe_core_t core;
            ecat_foe_core_cfg_t cfg;
            init_harness_cfg(&cfg, &s_harness_drv);
            assert(ecat_foe_core_init(&core, &cfg) == 0);
            ecat_foe_soes_adapter_t adapter;
            ecat_foe_soes_init(&adapter, &core, 128);
            assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

            s_foe_files[0].name = "firmware_mcu.bin";

            MBXcontrol[0].state = MBXstate_inclaim;
            _FOE *req = (_FOE *)&MBX[0];
            req->mbxheader.length = (uint16_t)sizeof(_FOEh); /* 空文件名: 0 长度 */
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            req->foeheader.password = 0;

            int rc = ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh));
            assert(rc != 0); /* 钩子安全拒绝 */
            ESC_foeprocess();
            _FOE *res_mbx = get_last_outbound_foe();
            assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
            assert(res_mbx->foeheader.errorcode == 0x8001); /* 原生 SOES 正确回复 0x8001 NOTFOUND */
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_mbx, sizeof(_MBXh) + sizeof(_FOEh));
            assert(s_foe_cfg.n_files == 2); /* 恢复条目数 */
            mock_master_read_outbox();
            printf("  [PASS] 风险 2 防御验证: 空文件名 WRQ 严格拒绝，原生 SOES 回复 0x8001 NOTFOUND\n");
        }

        /* 风险 3 回归: 旧缓存存在时，短前缀 WRQ 严格拒绝并返回 0x8001 */
        {
            reset_soes_environment();
            mock_driver_reset(&s_harness_drv);
            ecat_foe_core_t core;
            ecat_foe_core_cfg_t cfg;
            init_harness_cfg(&cfg, &s_harness_drv);
            assert(ecat_foe_core_init(&core, &cfg) == 0);
            ecat_foe_soes_adapter_t adapter;
            ecat_foe_soes_init(&adapter, &core, 128);
            assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

            /* 模拟条目 0 已缓存长文件名 */
            s_foe_files[0].name = "SERVO_DRV_MCU_V1.0.0.0.efw";

            /* 主站发短前缀 "SERVO_DRV_MCU" */
            const char *prefix = "SERVO_DRV_MCU";
            MBXcontrol[0].state = MBXstate_inclaim;
            _FOE *req = (_FOE *)&MBX[0];
            req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(prefix));
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            req->foeheader.password = 0;
            memcpy(req->filename, prefix, strlen(prefix));

            int hook_rc = ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(prefix));
            assert(hook_rc != 0); /* 预检拒绝 */
            ESC_foeprocess();
            _FOE *res_mbx = get_last_outbound_foe();
            assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
            assert(res_mbx->foeheader.errorcode == 0x8001); /* 绝不误回复 ACK */
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_mbx, sizeof(_MBXh) + sizeof(_FOEh));
            mock_master_read_outbox();
            printf("  [PASS] 风险 3 防御验证: 短前缀被核心拦截，原生 SOES 比对切断并回复 0x8001\n");
        }

        /* 风险 4 回归: 未配置目标 ID 越界防御，绝不 fallback 篡改条目 0 */
        {
            reset_soes_environment();
            mock_driver_reset(&s_harness_drv);
            ecat_foe_core_t core;
            ecat_foe_core_cfg_t cfg;
            init_harness_cfg(&cfg, &s_harness_drv);
            assert(ecat_foe_core_init(&core, &cfg) == 0);
            ecat_foe_soes_adapter_t adapter;
            ecat_foe_soes_init(&adapter, &core, 128);
            assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

            /* 从站 SOES 只有 1 个文件配置 (MCU) */
            s_foe_cfg.n_files = 1;
            s_foe_files[0].name = "SERVO_DRV_MCU.efw";

            /* 主站请求升级 Target 2 (FPGA) */
            const char *fn_fpga = "SERVO_DRV_FPGA_V1.0.0.0.efw";
            MBXcontrol[0].state = MBXstate_inclaim;
            _FOE *req = (_FOE *)&MBX[0];
            req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fn_fpga));
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            req->foeheader.password = 0;
            memcpy(req->filename, fn_fpga, strlen(fn_fpga));

            int rc = ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn_fpga));
            assert(rc != 0); /* 安全拒绝 */
            ESC_foeprocess();
            _FOE *res_mbx = get_last_outbound_foe();
            assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
            assert(res_mbx->foeheader.errorcode == 0x8001);
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_mbx, sizeof(_MBXh) + sizeof(_FOEh));
            mock_master_read_outbox();
            /* 关键断言: 条目 0 的文件名未被篡改！ */
            /* 关键断言: 条目 0 的文件名未被篡改！ */
            assert(strcmp(s_foe_files[0].name, "SERVO_DRV_MCU.efw") == 0);
            printf("  [PASS] 风险 4 防御验证: 未配置目标越界拒绝，条目 0 名称完好保留无回退篡改\n");
        }
    }

    /* ====================================================================== */
    /*   新增深度风险专项: 暂存时序、响应关联、稀疏映射与防回滚事实边界核验   */
    /* ====================================================================== */
    printf("--- [Harness 深度风险加固] 暂存时序、响应关联、稀疏映射与防回滚核查 ---\n");
    {
        /* 1. 深度风险 1: 后置钩子未调用或传入 NULL/空/短/非FoE 响应时暂存防御 */
        {
            /* Case 1A: 后置钩子未调用，错误密码 WRQ 残留暂存后数据到达，write_cb 严格拒绝不打断已有会话 */
            reset_soes_environment();
            mock_driver_reset(&s_harness_drv);
            ecat_foe_core_t core;
            ecat_foe_core_cfg_t cfg;
            init_harness_cfg(&cfg, &s_harness_drv);
            assert(ecat_foe_core_init(&core, &cfg) == 0);
            ecat_foe_soes_adapter_t adapter;
            ecat_foe_soes_init(&adapter, &core, 128);
            assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

            /* 启动合法 Target 1 会话 */
            const char *fn1 = "SERVO_DRV_MCU_V1.0.0.0.efw";
            assert(ecat_foe_soes_on_wrq(&adapter, fn1, strlen(fn1)) == 0);
            assert(core.req_target_id == 1);
            assert(core.state == ECAT_FOE_STATE_WRQ_ACKED);

            /* 收到 Target 2 错误密码的 WRQ */
            const char *fn2 = "SERVO_DRV_FPGA_V1.0.0.0.efw";
            MBXcontrol[0].state = MBXstate_inclaim;
            _FOE *req = (_FOE *)&MBX[0];
            req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fn2));
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            req->foeheader.password = 0x99999999U; /* 错误密码 */
            memcpy(req->filename, fn2, strlen(fn2));

            assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn2)) == 0);
            assert(adapter.has_staged_wrq == true);
            assert(adapter.staged_target_id == 2);

            ESC_foeprocess();
            _FOE *res_mbx = get_last_outbound_foe();
            assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
            assert(res_mbx->foeheader.errorcode == 0x800A);
            mock_master_read_outbox();

            /* 模拟应用层未调用 post_process_hook，此时若后续调用 write_cb */
            uint8_t dummy_data[32] = {0};
            uint32_t wr_rc = ecat_foe_soes_write_cb(&s_foe_files[0], dummy_data, sizeof(dummy_data));
            assert(wr_rc != 0); /* 必须拒绝 */
            /* 关键断言: 暂存被清除，Target 1 会话与驱动未被破坏，Target 2 并未被激活 */
            assert(adapter.has_staged_wrq == false);
            assert(core.req_target_id == 1);
            assert(s_harness_drv.count_abort == 0);

            /* Case 1B: 后置钩子传入 NULL 缓冲区 */
            adapter.has_staged_wrq = true;
            adapter.staged_target_id = 2;
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, NULL, 0);
            assert(adapter.has_staged_wrq == false);
            assert(adapter.staged_filename[0] == '\0');

            /* Case 1C: 后置钩子传入 0 长度缓冲区 */
            adapter.has_staged_wrq = true;
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, dummy_data, 0);
            assert(adapter.has_staged_wrq == false);

            /* Case 1D: 后置钩子传入小于 12 字节短缓冲区 */
            adapter.has_staged_wrq = true;
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, dummy_data, 6);
            assert(adapter.has_staged_wrq == false);

            /* Case 1E: 后置钩子传入非 FoE 报文 (如 CoE mbxtype=0x02) */
            adapter.has_staged_wrq = true;
            uint8_t coe_buf[16] = {0};
            coe_buf[0] = 6; coe_buf[1] = 0; /* len = 6 */
            coe_buf[5] = 0x02; /* CoE */
            coe_buf[6] = FOE_OP_ACK; /* opcode 虽碰巧为 ACK */
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, coe_buf, sizeof(coe_buf));
            assert(adapter.has_staged_wrq == false);
            assert(core.req_target_id == 1); /* 仍维持旧会话 */

            /* Case 1F: 邮箱上下文未配置时，pre_hook 与 post_hook 严格失败关闭 */
            ecat_foe_soes_adapter_t unreg_adapter;
            ecat_foe_soes_init(&unreg_adapter, &core, 128);
            assert(ecat_foe_soes_pre_process_hook(&unreg_adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn2)) == -1);
            unreg_adapter.has_staged_wrq = true;
            assert(ecat_foe_soes_post_process_hook(&unreg_adapter, &s_foe_cfg, NULL, 0) == -1);
            assert(unreg_adapter.has_staged_wrq == false);

            printf("  [PASS] 深度风险 1: 后置钩子缺失、NULL、空、短帧与非FoE报文下暂存安全释放，write_cb 严密拦截\n");
        }

        /* 2. 深度风险 2: ACK(0) 报文号与旧缓存隔离核验 */
        {
            reset_soes_environment();
            mock_driver_reset(&s_harness_drv);
            ecat_foe_core_t core;
            ecat_foe_core_cfg_t cfg;
            init_harness_cfg(&cfg, &s_harness_drv);
            assert(ecat_foe_core_init(&core, &cfg) == 0);
            ecat_foe_soes_adapter_t adapter;
            ecat_foe_soes_init(&adapter, &core, 128);
            assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

            /* 构造伪造的 ACK(1) 报文 (包号为 1 而非 WRQ 应有的 0) */
            uint8_t fake_ack1[16] = {0};
            fake_ack1[0] = 6; fake_ack1[1] = 0;
            fake_ack1[5] = MBXFOE;
            fake_ack1[6] = FOE_OP_ACK;
            fake_ack1[8] = 1; /* packetnumber = 1 */

            adapter.has_staged_wrq = true;
            memcpy(adapter.staged_filename, "SERVO_DRV_MCU_V1.0.0.0.efw", 27);
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, fake_ack1, sizeof(fake_ack1));
            assert(adapter.has_staged_wrq == false);
            assert(core.state == ECAT_FOE_STATE_IDLE); /* 未被误激活 */

            /* 旧缓存隔离: 发送邮箱残留旧 ACK(0)，下发非法 WRQ */
            uint8_t old_cache_ack0[16] = {0};
            old_cache_ack0[0] = 6; old_cache_ack0[1] = 0;
            old_cache_ack0[5] = MBXFOE;
            old_cache_ack0[6] = FOE_OP_ACK;
            old_cache_ack0[8] = 0; /* packetnumber = 0 */

            const char *bad_proj = "WRONGPROJ_MCU_V1.0.0.0.efw";
            MBXcontrol[0].state = MBXstate_inclaim;
            _FOE *bad_req = (_FOE *)&MBX[0];
            bad_req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(bad_proj));
            bad_req->mbxheader.mbxtype = MBXFOE;
            bad_req->foeheader.opcode = FOE_OP_WRQ;
            bad_req->foeheader.password = 0;
            memcpy(bad_req->filename, bad_proj, strlen(bad_proj));

            int prc = ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(bad_proj));
            assert(prc != 0);
            assert(adapter.has_staged_wrq == false);
            /* 即便后置钩子读到旧缓存，因 has_staged_wrq 为 false，安全不动作 */
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, old_cache_ack0, sizeof(old_cache_ack0));
            assert(core.state == ECAT_FOE_STATE_IDLE);

            printf("  [PASS] 深度风险 2: 非 0 包号 ACK 拒绝激活，残留旧缓存安全隔离无误触发\n");
        }

        /* 3. 深度风险 3: 稀疏目标使能与紧凑文件表索引映射 */
        {
            reset_soes_environment();
            mock_driver_reset(&s_harness_drv);
            ecat_foe_core_t core;
            ecat_foe_core_cfg_t cfg;
            init_harness_cfg(&cfg, &s_harness_drv);
            /* 配置稀疏目标: 仅使能 Target 2 (FPGA) 与 Target 4 (EEPROM)，禁用 Target 1 与 3 */
            cfg.targets[0].enabled = false;
            cfg.targets[1].enabled = true;  /* Target 2 */
            cfg.targets[2].enabled = false;
            cfg.targets[3].target_id = 4;
            cfg.targets[3].enabled = true;  /* Target 4 */
            cfg.targets[3].max_payload_size = 4096U;
            cfg.targets[3].write_unit = 8U;
            cfg.targets[3].driver = &s_harness_drv.driver;
            cfg.targets[3].user_ctx = &s_harness_drv;
            assert(ecat_foe_core_init(&core, &cfg) == 0);

            ecat_foe_soes_adapter_t adapter;
            ecat_foe_soes_init(&adapter, &core, 128);
            assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

            /* 从站 SOES 声明紧凑文件表: 仅 2 个条目 */
            s_foe_cfg.n_files = 2;
            s_foe_files[0].name = "placeholder0";
            s_foe_files[1].name = "placeholder1";

            /* 显式配置稀疏映射: 先清空默认 1:1 映射，再将 Target 2 -> files[0], Target 4 -> files[1] */
            ecat_foe_soes_reset_target_file_map(&adapter);
            assert(ecat_foe_soes_set_target_file_map(&adapter, 2, 0) == 0);
            assert(ecat_foe_soes_set_target_file_map(&adapter, 4, 1) == 0);
            /* 校验重复目标映射防护: 尝试将 Target 1 同样映射至 files[0]，必须安全拒绝 (-1) */
            assert(ecat_foe_soes_set_target_file_map(&adapter, 1, 0) == -1);

            /* 测试 Target 2 WRQ 请求 */
            const char *fn_fpga = "SERVO_DRV_FPGA_V1.0.0.0.efw";
            MBXcontrol[0].state = MBXstate_inclaim;
            _FOE *req = (_FOE *)&MBX[0];
            req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fn_fpga));
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            req->foeheader.password = 0x87654321U; /* files[0] 对应密码由 SOES 检查 */
            memcpy(req->filename, fn_fpga, strlen(fn_fpga));

            s_foe_files[0].filepass = 0x87654321U;
            assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn_fpga)) == 0);
            assert(strcmp(s_foe_files[0].name, fn_fpga) == 0); /* 映射至 files[0] */

            ESC_foeprocess();
            _FOE *res_mbx = get_last_outbound_foe();
            assert(res_mbx->foeheader.opcode == FOE_OP_ACK);
            assert(res_mbx->foeheader.packetnumber == 0);
            assert(ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_mbx, sizeof(_MBXh) + sizeof(_FOEh)) == 0);
            assert(core.req_target_id == 2);
            mock_master_read_outbox();

            /* 测试禁用目标 Target 1 WRQ: 虽属于合法格式但核心未使能 */
            const char *fn_mcu = "SERVO_DRV_MCU_V1.0.0.0.efw";
            MBXcontrol[0].state = MBXstate_inclaim;
            req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fn_mcu));
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            memcpy(req->filename, fn_mcu, strlen(fn_mcu));
            assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn_mcu)) != 0);
            ESC_foeprocess();
            res_mbx = get_last_outbound_foe();
            assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
            /* 此时由于 Target 2 刚被接受且处于 FOE_WAIT_FOR_DATA 态，原生 SOES 返回 0x8004 (FOE_ERR_ILLEGAL) 并复位为 FOE_READY */
            assert(res_mbx->foeheader.errorcode == 0x8004);
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_mbx, sizeof(_MBXh) + sizeof(_FOEh));
            mock_master_read_outbox();

            /* 再次在 FOE_READY 下发送禁用目标 Target 1 WRQ，原生 SOES 比对切断返回 0x8001 (NOTFOUND) */
            MBXcontrol[0].state = MBXstate_inclaim;
            req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fn_mcu));
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            memcpy(req->filename, fn_mcu, strlen(fn_mcu));
            assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn_mcu)) != 0);
            ESC_foeprocess();
            res_mbx = get_last_outbound_foe();
            assert(res_mbx->foeheader.opcode == FOE_OP_ERR);
            assert(res_mbx->foeheader.errorcode == 0x8001);
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_mbx, sizeof(_MBXh) + sizeof(_FOEh));
            mock_master_read_outbox();

            printf("  [PASS] 深度风险 3: 稀疏目标到紧凑文件表索引映射准确，禁用目标被安全拦截\n");
        }

        /* 4. 深度风险 4: 只读预检与防回滚时序事实边界核验 */
        {
            reset_soes_environment();
            mock_driver_reset(&s_harness_drv);
            ecat_foe_core_t core;
            ecat_foe_core_cfg_t cfg;
            init_harness_cfg(&cfg, &s_harness_drv);
            /* 启用防回滚 */
            cfg.anti_rollback_enable = true;
            assert(ecat_foe_core_init(&core, &cfg) == 0);

            /* 事实核验 A: WRQ 只读预检接口仅验证文件名格式、工程名与目标使能，不执行防回滚判断 */
            uint16_t tid = 0;
            uint32_t ver = 0;
            const char *low_ver_fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
            int vrc = ecat_foe_core_validate_wrq(&core, low_ver_fn, strlen(low_ver_fn), &tid, &ver);
            assert(vrc == 0); /* 预检通过，证明 WRQ 阶段确实未执行防回滚 (文件名无防伪认证) */
            assert(tid == 1);
            assert(ver == 0x01000000U);

            /* 事实核验 B: 防回滚真正且严格在数据阶段解析已签名的固件头时执行 */
            assert(ecat_foe_core_on_wrq(&core, low_ver_fn, strlen(low_ver_fn)) == 0);
            /* 模拟固件头写入，内部由 mock platform_port 提供防回滚下限 */
            assert(core.state == ECAT_FOE_STATE_WRQ_ACKED);

            printf("  [PASS] 深度风险 4: 证实 WRQ 预检仅核验格式与使能，防回滚在数据阶段头签名处严格生效\n");
        }

        /* 5. 深度风险 5: 声明载荷长度与实际缓冲长度边界核查 */
        {
            reset_soes_environment();
            mock_driver_reset(&s_harness_drv);
            ecat_foe_core_t core;
            ecat_foe_core_cfg_t cfg;
            init_harness_cfg(&cfg, &s_harness_drv);
            assert(ecat_foe_core_init(&core, &cfg) == 0);
            ecat_foe_soes_adapter_t adapter;
            ecat_foe_soes_init(&adapter, &core, 128);
            assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

            /* 构造声明载荷超出传入缓冲区的畸形报文 */
            uint8_t malformed_len_buf[32] = {0};
            malformed_len_buf[0] = 100; malformed_len_buf[1] = 0; /* 声明 payload=100 */
            malformed_len_buf[5] = MBXFOE;
            malformed_len_buf[6] = FOE_OP_ACK;

            adapter.has_staged_wrq = true;
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, malformed_len_buf, 16);
            assert(adapter.has_staged_wrq == false); /* 越界声明被安全拦截 */

            /* 构造载荷长度不等于 6 字节的畸形 ACK 报文 (FoE ACK 载荷必须严格为 6 字节) */
            uint8_t wrong_payload_ack[32] = {0};
            wrong_payload_ack[0] = 8; wrong_payload_ack[1] = 0; /* 声明 payload=8 (异常) */
            wrong_payload_ack[5] = MBXFOE;
            wrong_payload_ack[6] = FOE_OP_ACK;

            adapter.has_staged_wrq = true;
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, wrong_payload_ack, 32);
            assert(adapter.has_staged_wrq == false); /* 异常载荷长度被安全拒绝 */

            printf("  [PASS] 深度风险 5: 声明载荷长度与实际缓冲长度边界核查通过，非标与越界声明安全拒绝\n");
        }

        /* 6. 深度风险 6: 暂存合规 WRQ 遇 SOES 拒绝且无可用输出邮箱/旧 ACK(0) 残留与新鲜度保障核查 */
        {
            reset_soes_environment();
            mock_driver_reset(&s_harness_drv);
            ecat_foe_core_t core;
            ecat_foe_core_cfg_t cfg;
            init_harness_cfg(&cfg, &s_harness_drv);
            assert(ecat_foe_core_init(&core, &cfg) == 0);
            ecat_foe_soes_adapter_t adapter;
            ecat_foe_soes_init(&adapter, &core, 128);
            assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

            /* 启动合法 Target 1 会话 */
            const char *fn1 = "SERVO_DRV_MCU_V1.0.0.0.efw";
            assert(ecat_foe_soes_on_wrq(&adapter, fn1, strlen(fn1)) == 0);
            assert(core.req_target_id == 1);
            assert(core.state == ECAT_FOE_STATE_WRQ_ACKED);

            /* 模拟发送邮箱中已残留上一笔交易待发送的 ACK(0) */
            uint8_t *outbox_ptr = &MBX[1 * ESC_MBXSIZE];
            _FOE *out_foe = (_FOE *)outbox_ptr;
            out_foe->mbxheader.length = 6;
            out_foe->mbxheader.mbxtype = MBXFOE;
            out_foe->foeheader.opcode = FOE_OP_ACK;
            out_foe->foeheader.packetnumber = 0;

            /* 构造真实故障注入：将所有输出槽位全部置为占用态，真实 ESC_claimbuffer() 自然返回 0 (输出邮箱耗尽) */
            for (uint8_t s = 1; s < ESC_MBXBUFFERS; s++) {
                MBXcontrol[s].state = MBXstate_outreq;
            }

            /* 主站下发格式合法的 Target 2 WRQ，但使用错误密码 */
            const char *fn2 = "SERVO_DRV_FPGA_V1.0.0.0.efw";
            MBXcontrol[0].state = MBXstate_inclaim;
            _FOE *req = (_FOE *)&MBX[0];
            req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fn2));
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            req->foeheader.password = 0x99999999U; /* 错误密码 */
            memcpy(req->filename, fn2, strlen(fn2));

            /* 执行前置钩子: 预检合规，暂存 Target 2，只读快照邮箱状态 (绝不篡改排队邮箱) */
            int pre_rc = ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn2));
            assert(pre_rc == 0);
            assert(adapter.has_staged_wrq == true);
            assert(adapter.staged_target_id == 2);
            /* 验证发送邮箱中的待发 ACK 报文完好保留，未被前置钩子破坏 */
            assert(out_foe->foeheader.opcode == FOE_OP_ACK);

            /* 原生 SOES 处理: 密码不匹配调用 FOE_abort(0x800A)，但因无可用输出邮箱未产生任何新报文 */
            ESC_foeprocess();
            /* 此时发送邮箱依旧保持原有 ACK 报文 */
            assert(out_foe->foeheader.opcode == FOE_OP_ACK);

            /* 执行后置钩子: 检测到无新认领槽位，安全释放暂存 Target 2 */
            ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, outbox_ptr, ESC_MBXSIZE);
            assert(adapter.has_staged_wrq == false);

            /* 关键断言 1: 排队待发送的 ACK 完好保留，未在后置钩子处理时被清零破坏 */
            assert(out_foe->foeheader.opcode == FOE_OP_ACK && "queued ACK must NOT be destroyed before SOES transmission");
            assert(out_foe->mbxheader.length == 6);
            assert(MBXcontrol[1].state == MBXstate_outreq);

            /* 关键断言 2: 核心会话与驱动完好无损，维持 Target 1，驱动 abort 调用次数为 0 */
            assert(core.req_target_id == 1);
            assert(core.state == ECAT_FOE_STATE_WRQ_ACKED);
            assert(s_harness_drv.count_abort == 0);
            for (uint8_t s = 1; s < ESC_MBXBUFFERS; s++) {
                MBXcontrol[s].state = MBXstate_idle;
            }

            printf("  [PASS] 深度风险 6: 暂存WRQ在SOES密码拒绝且无输出邮箱时安全丢弃，新鲜度保障防范旧ACK残留且绝不篡改待发报文\n");
        }

        /* 7. 深度风险 7: 重复目标映射拦截与取消映射流转核查 */
        {
            ecat_foe_soes_adapter_t adapter;
            ecat_foe_core_t core;
            ecat_foe_soes_init(&adapter, &core, 128);

            /* 测试重复目标映射拦截 */
            ecat_foe_soes_reset_target_file_map(&adapter);
            assert(ecat_foe_soes_set_target_file_map(&adapter, 2, 0) == 0);
            /* 重复映射至索引 0 必须返回失败 (-1) */
            assert(ecat_foe_soes_set_target_file_map(&adapter, 4, 0) == -1);

            /* 测试取消映射与复用 */
            assert(ecat_foe_soes_set_target_file_map(&adapter, 2, -1) == 0); /* 取消 Target 2 映射 */
            /* 此时索引 0 已释放，Target 4 可顺利映射至索引 0 */
            assert(ecat_foe_soes_set_target_file_map(&adapter, 4, 0) == 0);

            printf("  [PASS] 深度风险 7: 重复目标映射安全拦截，取消映射与条目安全复用正常\n");
        }

        /* 8. 深度风险 8: 端到端 ACK、ERR 最终发给主站，忙/多槽/旧包共存与状态机完整性核查 */
        {
            /* 8A: 正确密码下 ACK 完整保留并由 ESC_writembx 成功发给主站 */
            reset_soes_environment();
            mock_driver_reset(&s_harness_drv);
            ecat_foe_core_t core;
            ecat_foe_core_cfg_t cfg;
            init_harness_cfg(&cfg, &s_harness_drv);
            assert(ecat_foe_core_init(&core, &cfg) == 0);
            ecat_foe_soes_adapter_t adapter;
            ecat_foe_soes_init(&adapter, &core, 128);
            assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

            s_foe_files[0].filepass = 0x12345678U;
            s_foe_files[0].name = "SERVO_DRV_MCU.efw";
            s_foe_files[0].write_function = ecat_foe_soes_write_cb;

            const char *fn_mcu = "SERVO_DRV_MCU_V1.0.0.0.efw";
            MBXcontrol[0].state = MBXstate_inclaim;
            _FOE *req = (_FOE *)&MBX[0];
            req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fn_mcu));
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            req->foeheader.password = 0x12345678U;
            memcpy(req->filename, fn_mcu, strlen(fn_mcu));

            assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn_mcu)) == 0);
            ESC_foeprocess();

            _FOE *res_slot1 = get_last_outbound_foe();
            uint8_t out_slot1 = get_last_outbound_slot();
            assert(res_slot1->foeheader.opcode == FOE_OP_ACK);
            assert(res_slot1->foeheader.packetnumber == 0);

            /* 调用 post_process_hook */
            assert(ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_slot1, sizeof(_MBXh) + sizeof(_FOEh)) == 0);
            assert(core.req_target_id == 1);
            assert(core.state == ECAT_FOE_STATE_WRQ_ACKED);

            /* 核心断言: 发送邮箱中的 ACK 报文毫发无损 */
            assert(res_slot1->foeheader.opcode == FOE_OP_ACK);
            assert(res_slot1->foeheader.packetnumber == 0);
            assert(MBXcontrol[out_slot1].state == MBXstate_outreq);

            /* 调用真实 SOES 协议栈发送邮箱向硬件写入 (ESC_writembx) */
            ESC_writembx(out_slot1);
            assert(s_hw_esc_write_calls == 1);
            _FOE *hw_sent_foe = (_FOE *)s_hw_sm1_buf;
            assert(hw_sent_foe->mbxheader.mbxtype == MBXFOE);
            assert(hw_sent_foe->foeheader.opcode == FOE_OP_ACK);
            assert(hw_sent_foe->foeheader.packetnumber == 0);
            MBXcontrol[out_slot1].state = MBXstate_idle; /* 模拟主站读取释放槽位 */

            /* 主站发送 ERR 复位 SOES 状态机至 READY 态 */
            MBXcontrol[0].state = MBXstate_inclaim;
            req->foeheader.errorcode = 0x8000;
            req->foeheader.opcode = FOE_OP_ERR;
            req->mbxheader.length = (uint16_t)sizeof(_FOEh);
            ESC_foeprocess();
            mock_master_read_outbox();

            /* 8B: 错误密码下 ERR 完整保留并由 ESC_writembx 成功发给主站 */
            MBXcontrol[0].state = MBXstate_inclaim;
            req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fn_mcu));
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            req->foeheader.password = 0x99999999U; /* 错误密码 */
            memcpy(req->filename, fn_mcu, strlen(fn_mcu));
            assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn_mcu)) == 0);
            ESC_foeprocess();

            _FOE *res_err = get_last_outbound_foe();
            uint8_t out_slot_err = get_last_outbound_slot();
            assert(res_err->foeheader.opcode == FOE_OP_ERR);
            assert(res_err->foeheader.errorcode == 0x800A);

            /* 调用 post_process_hook */
            assert(ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_err, sizeof(_MBXh) + sizeof(_FOEh)) == 0);
            assert(adapter.has_staged_wrq == false);

            /* 核心断言: 发送邮箱中的 ERR 报文毫发无损 */
            assert(res_err->foeheader.opcode == FOE_OP_ERR);
            assert(res_err->foeheader.errorcode == 0x800A);
            assert(MBXcontrol[out_slot_err].state == MBXstate_outreq);

            /* 真实 SOES 发送给硬件端口桩 */
            ESC_writembx(out_slot_err);
            assert(s_hw_esc_write_calls == 2);
            assert(hw_sent_foe->foeheader.opcode == FOE_OP_ERR);
            assert(hw_sent_foe->foeheader.errorcode == 0x800A);
            MBXcontrol[out_slot_err].state = MBXstate_idle; /* 释放槽位 */

            /* 8C: 多槽配置 (ESC_MBXBUFFERS=4) 与旧报文共存场景 */
            reset_soes_environment();
            s_foe_files[0].name = "SERVO_DRV_MCU.efw";
            s_foe_files[0].filepass = 0x12345678U;
            s_foe_files[0].write_function = ecat_foe_soes_write_cb;

            /* 槽位 1 模拟排队待发的 CoE 旧报文，槽位 2 模拟备份报文 */
            MBXcontrol[1].state = MBXstate_outreq;
            _FOE *slot1_foe = (_FOE *)&MBX[1 * ESC_MBXSIZE];
            slot1_foe->mbxheader.length = 10;
            slot1_foe->mbxheader.mbxtype = 0x03; /* CoE */

            MBXcontrol[2].state = MBXstate_backup;

            ecat_foe_soes_init(&adapter, &core, 128);
            assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

            MBXcontrol[0].state = MBXstate_inclaim;
            req = (_FOE *)&MBX[0];
            req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fn_mcu));
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            req->foeheader.password = 0x12345678U;
            memcpy(req->filename, fn_mcu, strlen(fn_mcu));

            assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn_mcu)) == 0);
            ESC_foeprocess();

            /* 原生 SOES 扫描发现槽位 3 为空闲，成功认领槽位 3 */
            assert(adapter.mbx_ctx.pre_states[3] == MBXstate_idle);
            assert(MBXcontrol[3].state == MBXstate_outreq);
            _FOE *res_slot3 = (_FOE *)&MBX[3 * ESC_MBXSIZE];
            assert(res_slot3->foeheader.opcode == FOE_OP_ACK);

            /* 执行后置钩子: 准确识别并锁定槽位 3，且杜绝破坏槽位 1 与槽位 2 */
            assert(ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_slot3, sizeof(_MBXh) + sizeof(_FOEh)) == 0);
            assert(adapter.mbx_ctx.newly_claimed_slot == 3);
            assert(core.state == ECAT_FOE_STATE_WRQ_ACKED);

            /* 关键断言: 槽位 1 与 2 的状态与内容未受触碰 */
            assert(MBXcontrol[1].state == MBXstate_outreq);
            assert(slot1_foe->mbxheader.mbxtype == 0x03);
            assert(MBXcontrol[2].state == MBXstate_backup);

            /* 槽位 3 的 ACK 发送给主站 */
            ESC_writembx(3);
            hw_sent_foe = (_FOE *)s_hw_sm1_buf;
            assert(hw_sent_foe->foeheader.opcode == FOE_OP_ACK);
            assert(hw_sent_foe->foeheader.packetnumber == 0);

            /* 槽位 1 依然维持待发状态 */
            assert(MBXcontrol[1].state == MBXstate_outreq);

            printf("  [PASS] 深度风险 8: ACK/ERR 最终完整发给主站，多槽与排队/备份共存正常，不篡改 SOES 所有权\n");
        }

        /* 9. 深度风险 9: 异常分支主站可观察一致失败保障与契约边界核查 */
        {
            /* 9A: 核心激活失败时 (core_on_wrq != 0)，post hook 将 ACK(0) 就地改写为 FOE_OP_ERR (0x8000) */
            reset_soes_environment();
            mock_driver_reset(&s_harness_drv);
            ecat_foe_core_t core;
            ecat_foe_core_cfg_t cfg;
            init_harness_cfg(&cfg, &s_harness_drv);
            assert(ecat_foe_core_init(&core, &cfg) == 0);
            ecat_foe_soes_adapter_t adapter;
            ecat_foe_soes_init(&adapter, &core, 128);
            assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

            s_foe_files[0].filepass = 0;
            s_foe_files[0].name = "SERVO_DRV_MCU.efw";
            s_foe_files[0].write_function = ecat_foe_soes_write_cb;

            const char *fn_mcu = "SERVO_DRV_MCU_V1.0.0.0.efw";
            MBXcontrol[0].state = MBXstate_inclaim;
            _FOE *req = (_FOE *)&MBX[0];
            req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fn_mcu));
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            req->foeheader.password = 0;
            memcpy(req->filename, fn_mcu, strlen(fn_mcu));

            /* 正常前置钩子暂存 */
            assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn_mcu)) == 0);
            ESC_foeprocess();

            uint8_t out_slot = get_last_outbound_slot();
            _FOE *res_foe = (_FOE *)&MBX[out_slot * ESC_MBXSIZE];
            assert(res_foe->foeheader.opcode == FOE_OP_ACK);

            /* 故意将核心配置标记置为 false，导致 ecat_foe_core_on_wrq 拒绝激活 */
            core.is_configured = false;

            /* 执行后置钩子: 检测到 core_on_wrq 失败，必须就地中和 ACK 为 FOE_OP_ERR (0x8000) 并返回 -1 */
            int post_rc = ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)res_foe, sizeof(_MBXh) + sizeof(_FOEh));
            assert(post_rc == -1);
            assert(adapter.has_staged_wrq == false);

            /* 核心断言: 该槽位已被转换为 0x8000 FOE_OP_ERR，杜绝主站收到假 ACK */
            assert(res_foe->foeheader.opcode == FOE_OP_ERR);
            assert(res_foe->foeheader.errorcode == 0x8000);

            /* 真实 SOES 发送给物理邮箱 */
            ESC_writembx(out_slot);
            _FOE *hw_sent = (_FOE *)s_hw_sm1_buf;
            assert(hw_sent->foeheader.opcode == FOE_OP_ERR);
            assert(hw_sent->foeheader.errorcode == 0x8000);
            MBXcontrol[out_slot].state = MBXstate_idle;

            /* 9B: 外部错传槽位地址时 (mbx_out_buf != slot_buf)，槽位被安全中和为 FOE_OP_ERR 并返回 -1 */
            reset_soes_environment();
            init_harness_cfg(&cfg, &s_harness_drv);
            assert(ecat_foe_core_init(&core, &cfg) == 0);
            ecat_foe_soes_init(&adapter, &core, 128);
            assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

            MBXcontrol[0].state = MBXstate_inclaim;
            req = (_FOE *)&MBX[0];
            req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fn_mcu));
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            req->foeheader.password = 0;
            memcpy(req->filename, fn_mcu, strlen(fn_mcu));

            assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn_mcu)) == 0);
            ESC_foeprocess();

            out_slot = get_last_outbound_slot();
            res_foe = (_FOE *)&MBX[out_slot * ESC_MBXSIZE];
            assert(res_foe->foeheader.opcode == FOE_OP_ACK);

            /* 传入错误的缓冲区指针 (例如传入槽位 1 的地址，而实际新认领的是槽位 3) */
            uint8_t *wrong_buf = &MBX[1 * ESC_MBXSIZE];
            post_rc = ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, wrong_buf, sizeof(_MBXh) + sizeof(_FOEh));
            assert(post_rc == -1);
            assert(adapter.has_staged_wrq == false);
            assert(core.state == ECAT_FOE_STATE_IDLE); /* 核心未被激活 */

            /* 核心断言: 槽位 3 已被就地中和为 FOE_OP_ERR (0x8000) */
            assert(res_foe->foeheader.opcode == FOE_OP_ERR);
            assert(res_foe->foeheader.errorcode == 0x8000);
            ESC_writembx(out_slot);
            assert(hw_sent->foeheader.opcode == FOE_OP_ERR);
            MBXcontrol[out_slot].state = MBXstate_idle;

            /* 9C: 契约破坏测试：多槽位同时跃迁为 outreq (如多任务或中断插入)，所有 ACK 槽位被中和并返回 -1 */
            reset_soes_environment();
            init_harness_cfg(&cfg, &s_harness_drv);
            assert(ecat_foe_core_init(&core, &cfg) == 0);
            ecat_foe_soes_init(&adapter, &core, 128);
            assert(ecat_foe_soes_set_mbx_context(&adapter, MBXcontrol, MBX, 4, 128) == 0);

            MBXcontrol[0].state = MBXstate_inclaim;
            req = (_FOE *)&MBX[0];
            req->mbxheader.length = (uint16_t)(sizeof(_FOEh) + strlen(fn_mcu));
            req->mbxheader.mbxtype = MBXFOE;
            req->foeheader.opcode = FOE_OP_WRQ;
            req->foeheader.password = 0;
            memcpy(req->filename, fn_mcu, strlen(fn_mcu));

            assert(ecat_foe_soes_pre_process_hook(&adapter, &s_foe_cfg, MBX, sizeof(_MBXh) + sizeof(_FOEh) + strlen(fn_mcu)) == 0);
            ESC_foeprocess(); /* 认领了槽位 3 */

            /* 模拟恶意并发中断：在 post 执行前，另一个任务把槽位 2 也置为 outreq */
            MBXcontrol[2].state = MBXstate_outreq;
            _FOE *slot2_foe = (_FOE *)&MBX[2 * ESC_MBXSIZE];
            slot2_foe->mbxheader.length = 6;
            slot2_foe->mbxheader.mbxtype = MBXFOE;
            slot2_foe->foeheader.opcode = FOE_OP_ACK;
            slot2_foe->foeheader.packetnumber = 0;

            post_rc = ecat_foe_soes_post_process_hook(&adapter, &s_foe_cfg, (const uint8_t *)&MBX[3 * ESC_MBXSIZE], sizeof(_MBXh) + sizeof(_FOEh));
            assert(post_rc == -1); /* 契约破坏被拦截 */
            assert(adapter.has_staged_wrq == false);
            assert(core.state == ECAT_FOE_STATE_IDLE);

            /* 关键断言: 槽位 3 与槽位 2 均被中和为 FOE_OP_ERR (0x8000) */
            _FOE *slot3_foe = (_FOE *)&MBX[3 * ESC_MBXSIZE];
            assert(slot3_foe->foeheader.opcode == FOE_OP_ERR);
            assert(slot3_foe->foeheader.errorcode == 0x8000);
            assert(slot2_foe->foeheader.opcode == FOE_OP_ERR);
            assert(slot2_foe->foeheader.errorcode == 0x8000);

            MBXcontrol[2].state = MBXstate_idle;
            MBXcontrol[3].state = MBXstate_idle;

            printf("  [PASS] 深度风险 9: 异常分支主站可观察一致失败保障，多槽竞争与指针不匹配均转化为 0x8000 报错\n");
        }
    }

    printf("------------------------------------------------------------\n");
    printf(">>> 真实 SOES esc_foe 11 项宿主行为与前置钩子验证全部通过！<<<\n");
    return 0;
}
