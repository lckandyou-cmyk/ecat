/**
 * @file test_soes_real_harness.c
 * @brief 真实 SOES esc_foe.c 宿主独立测试 Harness
 *
 * 验证目标:
 * 1. 首块回调时机核查: 验证真实 SOES 在 WRQ 阶段是否触发 write_function；
 * 2. 文件名前缀匹配核查: 验证真实 SOES FOE_fopen 中 strncmp(name, cfg_name, num_chars) 的前缀匹配机理；
 * 3. 零长度包返回值核查: 验证真实 SOES 在 data_len == 0 分支对 FOE_fclose() 返回值的忽略特性与短包 abort 行为对比；
 * 4. 适配层流式调用联动: 验证核心协议机在无 WRQ 直调或异常数据流下的防御行为。
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

/* Test 4: 适配层端到端数据接入保护核查 */
static void test_adapter_with_real_soes_interaction(void)
{
    printf("--- [Harness Test 4] 适配层与真实协议机联动防御核查 ---\n");
    reset_soes_environment();

    static mock_driver_t s_harness_drv;
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

    uint8_t dummy_chunk[64];
    memset(dummy_chunk, 0xA5, sizeof(dummy_chunk));

    /* 验证事实: 若真实 SOES 未调用 on_wrq 即调用数据写入，核心机拒绝并返回 -1 */
    int st = ecat_foe_core_on_data(&core, dummy_chunk, sizeof(dummy_chunk));
    assert(st == -1);
    assert(core.last_error == ECAT_FOE_CORE_ERR_INVALID_STATE);
    assert(s_harness_drv.count_init == 0);

    /* 适配层回调拒绝核验 */
    ecat_foe_soes_adapter_t adapter;
    ecat_foe_soes_init(&adapter, &core, 512);
    foe_file_cfg_t file_cfg;
    memset(&file_cfg, 0, sizeof(file_cfg));
    uint32_t cb_ret = ecat_foe_soes_write_cb(&file_cfg, dummy_chunk, sizeof(dummy_chunk));
    assert(cb_ret != 0);

    printf("  [PASS] 未经 WRQ 握手直接进入数据流时，核心机拒绝 (返回 -1, last_error=0x%04X)，适配层回调返回非零 (%u)\n",
           core.last_error, cb_ret);
}

int main(void)
{
    printf("============================================================\n");
    printf(" 真实 SOES esc_foe 宿主独立验证 Harness\n");
    printf("============================================================\n");

    test_real_soes_wrq_callback_timing();
    test_real_soes_filename_prefix_matching();
    test_real_soes_zero_len_vs_short_packet();
    test_adapter_with_real_soes_interaction();

    printf("------------------------------------------------------------\n");
    printf(">>> 真实 SOES esc_foe 4 项宿主行为验证全部通过！<<<\n");
    return 0;
}
