/**
 * @file test_end_to_end_multitransport.c
 * @brief 真实业务场景端到端测试: 多通道 + 多目标 + 权限矩阵 + 双向导入导出
 *
 * 场景还原:
 * 1. 系统包含 3 个传输通道:
 *    - 通道 1 (FoE): 工业总线通道
 *    - 通道 2 (UART): 本地调试串口通道
 *    - 通道 3 (CAN): 车辆/底盘控制总线通道
 * 2. 系统包含 4 个物理存储目标:
 *    - 目标 1: "mcu_fw" (固件区: 仅写, 大小 1MB)
 *    - 目标 2: "fpga_img" (固件区: 仅写, 大小 512KB)
 *    - 目标 3: "sys_params" (数据区: 读写 RW, 大小 64KB)
 *    - 目标 4: "flight_log" (数据区: 仅读 READ, 大小 128KB)
 * 3. 访问控制矩阵 (ACL):
 *    - FoE (1) 仅允许写 MCU 固件 (1) 和 FPGA (2)
 *    - UART (2) 仅允许读写系统参数 (3)
 *    - CAN (3) 仅允许导出黑匣子日志 (4)
 * 4. 验证内容:
 *    - 合法操作全部跑通并比对 CRC 与字节一致性
 *    - 越权与违规操作 (如 UART 篡改固件、CAN 写入日志、FoE 越权读取) 100% 被拦截
 */

#include "ecat_transport_endpoint.h"
#include "ecat_target_registry.h"
#include "ecat_transfer.h"
#include "ecat_foe_efw.h"
#include "ecat_foe_core_async.h"
#include "ecat_foe_soes.h"
#include "ecat_foe_ssc.h"
#include "ecat_foe_ssc_async.h"
#include "ecat_transfer_file.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

/* Mock 固件包构造器 */
static size_t build_e2e_firmware(uint8_t *out_buf, size_t buf_size, uint16_t target_id,
                                 uint32_t fw_ver, const uint8_t *payload, uint32_t payload_len)
{
    assert(buf_size >= ECAT_FOE_EFW_HEADER_MIN_LEN + payload_len);
    memset(out_buf, 0, ECAT_FOE_EFW_HEADER_MIN_LEN);
    out_buf[0] = 0x45; out_buf[1] = 0x43; out_buf[2] = 0x46; out_buf[3] = 0x57; /* ECFW */
    out_buf[4] = 0x02; out_buf[5] = 0x00;                                         /* v2 */
    out_buf[6] = 0x94; out_buf[7] = 0x00;                                         /* 148 字节头 */

    /* hardware_id = 1 */
    out_buf[12] = 0x01; out_buf[13] = 0x00; out_buf[14] = 0x00; out_buf[15] = 0x00;
    /* hw_ver_min = 0x0100, hw_ver_max = 0x0100 */
    out_buf[16] = 0x00; out_buf[17] = 0x01;
    out_buf[18] = 0x00; out_buf[19] = 0x01;

    out_buf[20] = (uint8_t)(target_id & 0xFF);
    out_buf[21] = (uint8_t)((target_id >> 8) & 0xFF);
    out_buf[22] = 0x00; out_buf[23] = 0x00;

    out_buf[24] = (uint8_t)(fw_ver & 0xFF);
    out_buf[25] = (uint8_t)((fw_ver >> 8) & 0xFF);
    out_buf[26] = (uint8_t)((fw_ver >> 16) & 0xFF);
    out_buf[27] = (uint8_t)((fw_ver >> 24) & 0xFF);

    out_buf[32] = (uint8_t)(payload_len & 0xFF);
    out_buf[33] = (uint8_t)((payload_len >> 8) & 0xFF);
    out_buf[34] = (uint8_t)((payload_len >> 16) & 0xFF);
    out_buf[35] = (uint8_t)((payload_len >> 24) & 0xFF);

    uint32_t p_crc = (payload != NULL && payload_len > 0) ? ecat_foe_crc32_calc(payload, payload_len) : 0U;
    out_buf[36] = (uint8_t)(p_crc & 0xFF);
    out_buf[37] = (uint8_t)((p_crc >> 8) & 0xFF);
    out_buf[38] = (uint8_t)((p_crc >> 16) & 0xFF);
    out_buf[39] = (uint8_t)((p_crc >> 24) & 0xFF);

    memcpy(out_buf + 60, "SIG2", 4);
    out_buf[64] = 1U; /* flags: signed */
    out_buf[66] = 1U; /* algorithm: Ed25519ph */
    out_buf[68] = ECAT_FOE_EFW_SIGNATURE_LEN;
    out_buf[70] = ECAT_FOE_EFW_KEY_ID_LEN;
    for (size_t i = 0; i < ECAT_FOE_EFW_KEY_ID_LEN; ++i) out_buf[72U + i] = (uint8_t)(0x10U + i);
    memset(out_buf + 80U, 0xA5, ECAT_FOE_EFW_SIGNATURE_LEN);
    uint32_t h_crc = ecat_foe_crc32_calc(out_buf, 144U);
    out_buf[144] = (uint8_t)(h_crc & 0xFF);
    out_buf[145] = (uint8_t)((h_crc >> 8) & 0xFF);
    out_buf[146] = (uint8_t)((h_crc >> 16) & 0xFF);
    out_buf[147] = (uint8_t)((h_crc >> 24) & 0xFF);

    if (payload != NULL && payload_len > 0) {
        memcpy(out_buf + ECAT_FOE_EFW_HEADER_MIN_LEN, payload, payload_len);
    }
    return ECAT_FOE_EFW_HEADER_MIN_LEN + payload_len;
}

/* Mock 签名验证器 (Ed25519 / Monocypher 流式核验) */
typedef struct {
    bool fail_finish;
    unsigned begin_calls;
    unsigned update_calls;
    unsigned finish_calls;
    unsigned abort_calls;
} test_e2e_sig_verifier_t;

static int e2e_sig_begin(const uint8_t key_id[ECAT_FOE_EFW_KEY_ID_LEN],
                         const uint8_t signature[ECAT_FOE_EFW_SIGNATURE_LEN],
                         void *user_ctx)
{
    (void)key_id; (void)signature;
    test_e2e_sig_verifier_t *v = (test_e2e_sig_verifier_t *)user_ctx;
    if (v) v->begin_calls++;
    return 0;
}

static int e2e_sig_update(const uint8_t *data, size_t len, void *user_ctx)
{
    (void)data; (void)len;
    test_e2e_sig_verifier_t *v = (test_e2e_sig_verifier_t *)user_ctx;
    if (v) v->update_calls++;
    return 0;
}

static int e2e_sig_finish(void *user_ctx)
{
    test_e2e_sig_verifier_t *v = (test_e2e_sig_verifier_t *)user_ctx;
    if (v) {
        v->finish_calls++;
        return v->fail_finish ? -1 : 0;
    }
    return 0;
}

static void e2e_sig_abort(void *user_ctx)
{
    test_e2e_sig_verifier_t *v = (test_e2e_sig_verifier_t *)user_ctx;
    if (v) v->abort_calls++;
}

/* Mock 异步数据驱动 (支持真实 payload 观察与硬件生命周期状态跟踪) */
typedef struct {
    bool hardware_active;          /* 硬件当前是否正在异步访问缓冲区 */
    const uint8_t *hw_reading_buf; /* 硬件当前绑定的缓冲区指针 */
    size_t hw_reading_len;
    uint8_t received_payload[256]; /* 硬件最终成功落盘的数据 */
    size_t received_len;
    unsigned start_calls;
    unsigned poll_calls;
    unsigned abort_calls;
} e2e_hw_async_state_t;

static e2e_hw_async_state_t s_e2e_hw_state;
static ecat_async_op_result_t s_e2e_async_res = ECAT_ASYNC_OP_COMPLETE;

static void e2e_hw_async_reset(void)
{
    memset(&s_e2e_hw_state, 0, sizeof(s_e2e_hw_state));
    s_e2e_async_res = ECAT_ASYNC_OP_COMPLETE;
}

static ecat_async_op_result_t e2e_async_start(void *ctx, ecat_async_data_op_t *op)
{
    (void)ctx;
    if (op != NULL) {
        op->actual_length = op->length;
        if (op->kind == ECAT_ASYNC_DATA_OP_ERASE_TO) {
            op->erased_end = op->need_end;
        }
        s_e2e_hw_state.start_calls++;
        if (op->kind == ECAT_ASYNC_DATA_OP_WRITE) {
            s_e2e_hw_state.hw_reading_buf = op->buffer;
            s_e2e_hw_state.hw_reading_len = op->length;
        }
        if (s_e2e_async_res == ECAT_ASYNC_OP_PENDING) {
            s_e2e_hw_state.hardware_active = true;
        } else if (s_e2e_async_res == ECAT_ASYNC_OP_COMPLETE) {
            s_e2e_hw_state.hardware_active = false;
            if (op->kind == ECAT_ASYNC_DATA_OP_WRITE && op->buffer != NULL &&
                op->length <= sizeof(s_e2e_hw_state.received_payload)) {
                memcpy(s_e2e_hw_state.received_payload, op->buffer, op->length);
                s_e2e_hw_state.received_len = op->length;
            }
            s_e2e_hw_state.hw_reading_buf = NULL;
        } else {
            s_e2e_hw_state.hardware_active = false;
            s_e2e_hw_state.hw_reading_buf = NULL;
        }
    }
    return s_e2e_async_res;
}

static ecat_async_op_result_t e2e_async_poll(void *ctx, ecat_async_data_op_t *op)
{
    (void)ctx;
    if (op != NULL) {
        op->actual_length = op->length;
        if (op->kind == ECAT_ASYNC_DATA_OP_ERASE_TO) {
            op->erased_end = op->need_end;
        }
        s_e2e_hw_state.poll_calls++;
        if (s_e2e_async_res == ECAT_ASYNC_OP_PENDING) {
            s_e2e_hw_state.hardware_active = true;
        } else if (s_e2e_async_res == ECAT_ASYNC_OP_COMPLETE) {
            s_e2e_hw_state.hardware_active = false;
            if (op->kind == ECAT_ASYNC_DATA_OP_WRITE && s_e2e_hw_state.hw_reading_buf != NULL &&
                s_e2e_hw_state.hw_reading_len <= sizeof(s_e2e_hw_state.received_payload)) {
                memcpy(s_e2e_hw_state.received_payload, s_e2e_hw_state.hw_reading_buf, s_e2e_hw_state.hw_reading_len);
                s_e2e_hw_state.received_len = s_e2e_hw_state.hw_reading_len;
            }
            s_e2e_hw_state.hw_reading_buf = NULL;
        } else {
            s_e2e_hw_state.hardware_active = false;
            s_e2e_hw_state.hw_reading_buf = NULL;
        }
    }
    return s_e2e_async_res;
}

static unsigned s_e2e_async_aborts = 0U;
static void e2e_async_abort(void *ctx)
{
    (void)ctx;
    s_e2e_async_aborts++;
    s_e2e_hw_state.abort_calls++;
    /* 硬件在 abort 调用时同步停止对缓冲区的访问 */
    s_e2e_hw_state.hardware_active = false;
    s_e2e_hw_state.hw_reading_buf = NULL;
}

int port_get_version_floor(uint16_t id, uint32_t *v)
{
    (void)id;
    if (v) *v = 0;
    return 0;
}

/* 模拟存储介质 */
static uint8_t s_flash_mcu[1024];
static uint8_t s_flash_params[256];
static uint8_t s_flash_log[512];

/* MCU 固件驱动 Mock */
static int mcu_init(void *ctx) { (void)ctx; return 0; }
static int mcu_invalidate(void *ctx) { (void)ctx; return 0; }
static int mcu_erase_to(uint32_t need_end, uint32_t *erased_end, void *ctx)
{
    (void)ctx;
    if (erased_end) *erased_end = need_end;
    return 0;
}
static int mcu_write(uint32_t offset, const uint8_t *data, uint32_t len, void *ctx)
{
    (void)ctx;
    if (offset + len > sizeof(s_flash_mcu)) return -1;
    memcpy(&s_flash_mcu[offset], data, len);
    return 0;
}
static int mcu_read(uint32_t offset, uint8_t *buf, uint32_t len, void *ctx)
{
    (void)ctx;
    if (offset + len > sizeof(s_flash_mcu)) return -1;
    memcpy(buf, &s_flash_mcu[offset], len);
    return 0;
}
static int mcu_finalize(void *ctx) { (void)ctx; return 0; }
static void mcu_abort(void *ctx) { (void)ctx; }

/* 参数区驱动 Mock */
static int params_init(void *ctx) { (void)ctx; return 0; }
static int params_read_open(void *ctx) { (void)ctx; return 0; }
static void params_read_close(void *ctx) { (void)ctx; }
static int params_erase_to(uint32_t need_end, uint32_t *erased_end, void *ctx)
{
    (void)ctx;
    if (erased_end) *erased_end = need_end;
    return 0;
}
static int params_write(uint32_t offset, const uint8_t *data, uint32_t len, void *ctx)
{
    (void)ctx;
    if (offset + len > sizeof(s_flash_params)) return -1;
    memcpy(&s_flash_params[offset], data, len);
    return 0;
}
static int params_read(uint32_t offset, uint8_t *buf, uint32_t len, void *ctx)
{
    (void)ctx;
    if (offset + len > sizeof(s_flash_params)) return -1;
    memcpy(buf, &s_flash_params[offset], len);
    return 0;
}
static int params_finalize(void *ctx) { (void)ctx; return 0; }
static void params_abort(void *ctx) { (void)ctx; }
static uint32_t params_get_size(void *ctx) { (void)ctx; return 128U; }

/* 日志区驱动 Mock */
static int log_init(void *ctx) { (void)ctx; return 0; }
static int log_read_open(void *ctx) { (void)ctx; return 0; }
static void log_read_close(void *ctx) { (void)ctx; }
static int log_read(uint32_t offset, uint8_t *buf, uint32_t len, void *ctx)
{
    (void)ctx;
    if (offset + len > sizeof(s_flash_log)) return -1;
    memcpy(buf, &s_flash_log[offset], len);
    return 0;
}
static uint32_t log_get_size(void *ctx) { (void)ctx; return sizeof(s_flash_log); }
static int log_finalize(void *ctx) { (void)ctx; return 0; }
static void log_abort(void *ctx) { (void)ctx; }

static uint8_t s_flash_fpga[1024];
static ecat_foe_target_async_result_t s_e2e_fw_async_res = ECAT_FOE_TARGET_ASYNC_COMPLETE;
static unsigned s_e2e_fw_async_aborts = 0U;
static unsigned s_e2e_fw_async_session_aborts = 0U;

static void e2e_fw_hw_reset(void)
{
    e2e_hw_async_reset();
    s_e2e_fw_async_res = ECAT_FOE_TARGET_ASYNC_COMPLETE;
    s_e2e_fw_async_aborts = 0U;
    s_e2e_fw_async_session_aborts = 0U;
}

static void e2e_fw_apply_complete(ecat_foe_target_async_op_t *op)
{
    if (op == NULL) return;
    if (op->kind == ECAT_FOE_TARGET_OP_ERASE_TO) {
        op->erased_end = op->need_end;
    } else if (op->kind == ECAT_FOE_TARGET_OP_WRITE && op->data != NULL) {
        if (op->offset + op->length <= sizeof(s_flash_fpga)) {
            memcpy(&s_flash_fpga[op->offset], op->data, op->length);
        }
        if (op->length <= sizeof(s_e2e_hw_state.received_payload)) {
            memcpy(s_e2e_hw_state.received_payload, op->data, op->length);
            s_e2e_hw_state.received_len = op->length;
        }
    } else if (op->kind == ECAT_FOE_TARGET_OP_READ && op->data != NULL) {
        if (op->offset + op->length <= sizeof(s_flash_fpga)) {
            memcpy(op->data, &s_flash_fpga[op->offset], op->length);
        }
    }
}

static ecat_foe_target_async_result_t e2e_fw_async_start(void *ctx, ecat_foe_target_async_op_t *op)
{
    (void)ctx;
    s_e2e_hw_state.start_calls++;
    if (s_e2e_fw_async_res == ECAT_FOE_TARGET_ASYNC_PENDING) {
        s_e2e_hw_state.hardware_active = true;
    } else if (s_e2e_fw_async_res == ECAT_FOE_TARGET_ASYNC_COMPLETE) {
        s_e2e_hw_state.hardware_active = false;
        e2e_fw_apply_complete(op);
    } else {
        s_e2e_hw_state.hardware_active = false;
    }
    return s_e2e_fw_async_res;
}

static ecat_foe_target_async_result_t e2e_fw_async_poll(void *ctx, ecat_foe_target_async_op_t *op)
{
    (void)ctx;
    s_e2e_hw_state.poll_calls++;
    if (s_e2e_fw_async_res == ECAT_FOE_TARGET_ASYNC_PENDING) {
        s_e2e_hw_state.hardware_active = true;
    } else if (s_e2e_fw_async_res == ECAT_FOE_TARGET_ASYNC_COMPLETE) {
        s_e2e_hw_state.hardware_active = false;
        e2e_fw_apply_complete(op);
    } else {
        s_e2e_hw_state.hardware_active = false;
    }
    return s_e2e_fw_async_res;
}

static void e2e_fw_async_abort(void *ctx)
{
    (void)ctx;
    s_e2e_fw_async_aborts++;
    s_e2e_hw_state.abort_calls++;
    s_e2e_hw_state.hardware_active = false;
    s_e2e_hw_state.hw_reading_buf = NULL;
}

static void e2e_fw_async_abort_session(void *ctx)
{
    (void)ctx;
    s_e2e_fw_async_session_aborts++;
    s_e2e_hw_state.hardware_active = false;
    s_e2e_hw_state.hw_reading_buf = NULL;
}

int main(void)
{
    printf("=== 开始运行端到端多通道多目标双向传输测试 ===\n");

    /* 1. 初始化目标注册表与权限矩阵 */
    ecat_target_registry_t reg;
    assert(ecat_target_registry_init(&reg) == 0);

    /* 注册目标 1: MCU 固件 (FIRMWARE 类型) */
    ecat_target_desc_t mcu_desc;
    memset(&mcu_desc, 0, sizeof(mcu_desc));
    mcu_desc.target_id = 1U;
    strncpy(mcu_desc.name, "mcu_fw", sizeof(mcu_desc.name));
    mcu_desc.type = ECAT_TARGET_TYPE_FIRMWARE;
    mcu_desc.permissions = ECAT_PERM_WRITE;
    mcu_desc.max_size = 1024U * 1024U;
    mcu_desc.write_unit = 8U;
    mcu_desc.enabled = true;
    mcu_desc.driver.io.init = mcu_init;
    mcu_desc.driver.io.invalidate = mcu_invalidate;
    mcu_desc.driver.io.erase_to = mcu_erase_to;
    mcu_desc.driver.io.write = mcu_write;
    mcu_desc.driver.io.read = mcu_read;
    mcu_desc.driver.io.finalize = mcu_finalize;
    mcu_desc.driver.io.abort = mcu_abort;
    assert(ecat_target_registry_add(&reg, &mcu_desc) == 0);

    /* 注册目标 3: 系统参数区 (DATA 类型, RW) */
    ecat_target_desc_t param_desc;
    memset(&param_desc, 0, sizeof(param_desc));
    param_desc.target_id = 3U;
    strncpy(param_desc.name, "sys_params", sizeof(param_desc.name));
    param_desc.type = ECAT_TARGET_TYPE_DATA;
    param_desc.permissions = ECAT_PERM_RW;
    param_desc.max_size = sizeof(s_flash_params);
    param_desc.enabled = true;
    param_desc.driver.read_open = params_read_open;
    param_desc.driver.read_close = params_read_close;
    param_desc.driver.io.init = params_init;
    param_desc.driver.io.erase_to = params_erase_to;
    param_desc.driver.io.write = params_write;
    param_desc.driver.io.read = params_read;
    param_desc.driver.io.finalize = params_finalize;
    param_desc.driver.io.abort = params_abort;
    param_desc.driver.get_data_size = params_get_size;
    assert(ecat_target_registry_add(&reg, &param_desc) == 0);

    /* 注册目标 4: 运行日志区 (DATA 类型, READ 仅读) */
    ecat_target_desc_t log_desc;
    memset(&log_desc, 0, sizeof(log_desc));
    log_desc.target_id = 4U;
    strncpy(log_desc.name, "flight_log", sizeof(log_desc.name));
    log_desc.type = ECAT_TARGET_TYPE_DATA;
    log_desc.permissions = ECAT_PERM_READ;
    log_desc.max_size = sizeof(s_flash_log);
    log_desc.enabled = true;
    log_desc.driver.read_open = log_read_open;
    log_desc.driver.read_close = log_read_close;
    log_desc.driver.io.init = log_init;
    log_desc.driver.io.read = log_read;
    log_desc.driver.io.finalize = log_finalize;
    log_desc.driver.io.abort = log_abort;
    log_desc.driver.get_data_size = log_get_size;
    assert(ecat_target_registry_add(&reg, &log_desc) == 0);

    /* 预填充日志区数据 */
    for (size_t i = 0; i < sizeof(s_flash_log); i++) {
        s_flash_log[i] = (uint8_t)(i ^ 0xA5);
    }

    /* 2. 配置严格 ACL 访问控制规则 */
    ecat_target_registry_set_enforce_acl(&reg, true);
    /* 规则 1: FoE (1) 仅允许写 MCU 固件 (1) */
    assert(ecat_target_registry_add_acl(&reg, 1U, 1U, ECAT_PERM_WRITE) == 0);
    /* 规则 2: UART (2) 仅允许读写系统参数 (3) */
    assert(ecat_target_registry_add_acl(&reg, 2U, 3U, ECAT_PERM_RW) == 0);
    /* 规则 3: CAN (3) 仅允许导出日志区 (4) */
    assert(ecat_target_registry_add_acl(&reg, 3U, 4U, ECAT_PERM_READ) == 0);

    /* 3. 初始化核心机与传输管理器 (共享唯一权威注册表 reg) */
    ecat_foe_core_t core;
    ecat_foe_core_cfg_t core_cfg;
    memset(&core_cfg, 0, sizeof(core_cfg));
    core_cfg.project_name = "DRIVE_SERVO";
    core_cfg.hardware_id = 0x0001U;
    core_cfg.local_hw_ver = 0x0100U;
    core_cfg.max_header_size = 148U;
    core_cfg.registry = &reg;
    assert(ecat_foe_core_init(&core, &core_cfg) == 0);

    ecat_transfer_t mgr;
    assert(ecat_transfer_init(&mgr, &reg, &core) == 0);

    /* 4. 绑定 3 个传输端点 */
    ecat_transport_endpoint_t ep_foe;
    ecat_transport_endpoint_t ep_uart;
    ecat_transport_endpoint_t ep_can;

    assert(ecat_transport_endpoint_init(&ep_foe, 1U, "ep_foe", &mgr, NULL) == 0);
    assert(ecat_transport_endpoint_init(&ep_uart, 2U, "ep_uart", &mgr, NULL) == 0);
    assert(ecat_transport_endpoint_init(&ep_can, 3U, "ep_can", &mgr, NULL) == 0);

    /* ---------------------------------------------------------------------- */
    /* 测试阶段 A: 越权攻击/未授权访问拦截                                   */
    /* ---------------------------------------------------------------------- */
    /* A1: UART 试图向 MCU 固件区刷写固件 -> 必须被 ACL 拦截 */
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 1U, 1024, 0x01000000U) == ECAT_TRANSFER_DENIED);

    /* A2: CAN 试图向日志区写入数据 -> 目标仅读且无 ACL -> 拦截 */
    assert(ecat_transport_endpoint_begin_import(&ep_can, 4U, 128, 0) == ECAT_TRANSFER_DENIED);

    /* A3: FoE 试图导出系统参数 -> 必须被 ACL 拦截 */
    assert(ecat_transport_endpoint_begin_export(&ep_foe, 3U, NULL) == ECAT_TRANSFER_DENIED);

    /* ---------------------------------------------------------------------- */
    /* 测试阶段 B: 合法业务流程执行                                           */
    /* ---------------------------------------------------------------------- */
    /* B1: UART 成功导入系统参数 */
    uint8_t new_params[128];
    for (size_t i = 0; i < sizeof(new_params); i++) {
        new_params[i] = (uint8_t)(i + 0x20);
    }
    uint32_t expected_param_crc = ecat_foe_crc32_calc(new_params, sizeof(new_params));

    assert(ecat_transport_endpoint_begin_import(&ep_uart, 3U, sizeof(new_params), 0) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, new_params, sizeof(new_params)) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_finish_import(&ep_uart) == ECAT_TRANSFER_COMPLETE);
    assert(memcmp(s_flash_params, new_params, sizeof(new_params)) == 0);

    /* B2: UART 成功回读验证系统参数 */
    uint32_t param_export_size = 0;
    assert(ecat_transport_endpoint_begin_export(&ep_uart, 3U, &param_export_size) == ECAT_TRANSFER_COMPLETE);
    uint8_t readback_params[128];
    size_t actual_rb = 0;
    assert(ecat_transport_endpoint_read_chunk(&ep_uart, readback_params, sizeof(readback_params), &actual_rb) == ECAT_TRANSFER_COMPLETE);
    assert(actual_rb == sizeof(new_params));
    assert(memcmp(readback_params, new_params, sizeof(new_params)) == 0);
    assert(ecat_transfer_get_crc32(&mgr) == expected_param_crc);
    assert(ecat_transport_endpoint_finish_export(&ep_uart) == ECAT_TRANSFER_COMPLETE);

    /* B3: CAN 成功导出黑匣子日志 */
    uint32_t log_size = 0;
    assert(ecat_transport_endpoint_begin_export(&ep_can, 4U, &log_size) == ECAT_TRANSFER_COMPLETE);
    assert(log_size == sizeof(s_flash_log));

    uint8_t rx_log[sizeof(s_flash_log)];
    size_t log_rx_total = 0;
    while (log_rx_total < log_size) {
        uint8_t chunk[64];
        size_t chunk_len = 0;
        ecat_transfer_result_t res = ecat_transport_endpoint_read_chunk(
            &ep_can, chunk, sizeof(chunk), &chunk_len);
        assert(chunk_len > 0);
        memcpy(&rx_log[log_rx_total], chunk, chunk_len);
        log_rx_total += chunk_len;
        if (log_rx_total == log_size) {
            assert(res == ECAT_TRANSFER_COMPLETE);
        } else {
            assert(res == ECAT_TRANSFER_CONTINUE);
        }
    }

    assert(memcmp(rx_log, s_flash_log, sizeof(s_flash_log)) == 0);
    uint32_t expected_log_crc = ecat_foe_crc32_calc(s_flash_log, sizeof(s_flash_log));
    assert(ecat_transfer_get_crc32(&mgr) == expected_log_crc);
    assert(ecat_transport_endpoint_finish_export(&ep_can) == ECAT_TRANSFER_COMPLETE);

    /* ---------------------------------------------------------------------- */
    /* 测试阶段 C: 跨通道会话互斥仲裁与异步挂起阻断验证     */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试阶段 C: 跨通道会话互斥仲裁与异步挂起阻断验证 ---\n");

    /* C1: 多通道同步互斥仲裁 */
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 3U, sizeof(new_params), 0) == ECAT_TRANSFER_COMPLETE);
    /* UART 处于活动状态，CAN 发起导出必须返回 BUSY */
    assert(ecat_transport_endpoint_begin_export(&ep_can, 4U, NULL) == ECAT_TRANSFER_BUSY);
    /* FoE 端点发起导入也必须返回 BUSY */
    assert(ecat_transport_endpoint_begin_import(&ep_foe, 1U, 1024, 0x01000000U) == ECAT_TRANSFER_BUSY);

    /* Direct callers and transport endpoints share the same manager. */
    int direct_owner = 0;
    ecat_transfer_request_t upg_req = { .transport_id = 1U, .target_id = 1U,
        .direction = ECAT_TRANSFER_DIR_IMPORT, .fw_version = 0x01000000U };
    uint64_t upg_token = 0;
    /* UART 端点占有会话时，直接请求 必须返回 BUSY */
    assert(ecat_transfer_begin(&mgr, &direct_owner, &upg_req, &upg_token) == ECAT_TRANSFER_BUSY);

    /* UART 端点取消并释放会话 */
    ecat_transport_endpoint_abort(&ep_uart);
    assert(ep_uart.current_token == 0U);

    /* C2: 注册异步目标 5，验证真正异步挂起期间全局租约阻断与释放 */
    ecat_async_data_ops_t e2e_async_ops = {
        .start = e2e_async_start,
        .poll = e2e_async_poll,
        .abort = e2e_async_abort
    };
    ecat_target_desc_t async_desc;
    memset(&async_desc, 0, sizeof(async_desc));
    async_desc.target_id = 5U;
    strncpy(async_desc.name, "async_storage", sizeof(async_desc.name));
    async_desc.type = ECAT_TARGET_TYPE_DATA;
    async_desc.permissions = ECAT_PERM_RW;
    async_desc.max_size = 1024U;
    async_desc.enabled = true;
    async_desc.asynchronous = true;
    async_desc.driver.async_ops = &e2e_async_ops;
    assert(ecat_target_registry_add(&reg, &async_desc) == 0);
    /* 添加 ACL: UART 允许写 5U */
    assert(ecat_target_registry_add_acl(&reg, 2U, 5U, ECAT_PERM_WRITE) == 0);

    /* 绑定暂存区 */
    uint8_t e2e_staging[256];
    assert(ecat_transfer_set_staging_buffer(&mgr, e2e_staging, sizeof(e2e_staging)) == 0);

    /* UART 发起异步写入 */
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 5U, 64, 0) == ECAT_TRANSFER_COMPLETE);
    s_e2e_async_res = ECAT_ASYNC_OP_PENDING;
    uint8_t async_chunk[64];
    memset(async_chunk, 0x55, sizeof(async_chunk));
    assert(ecat_transport_endpoint_write_chunk(&ep_uart, async_chunk, sizeof(async_chunk)) == ECAT_TRANSFER_PENDING);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == true);

    /* 异步 PENDING 期间，所有其它通道与直接请求必须被严格阻断 (BUSY) */
    assert(ecat_transport_endpoint_begin_export(&ep_can, 4U, NULL) == ECAT_TRANSFER_BUSY);
    assert(ecat_transport_endpoint_begin_import(&ep_foe, 1U, 1024, 0x01000000U) == ECAT_TRANSFER_BUSY);
    assert(ecat_transfer_begin(&mgr, &direct_owner, &upg_req, &upg_token) == ECAT_TRANSFER_BUSY);

    /* 推进 poll 完成数据写入 */
    s_e2e_async_res = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transport_endpoint_poll(&ep_uart) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transport_endpoint_is_pending(&ep_uart) == false);

    /* 终结提交 */
    assert(ecat_transport_endpoint_finish_import(&ep_uart) == ECAT_TRANSFER_COMPLETE);
    assert(ep_uart.current_token == 0U);

    /* C3: 会话释放后，直接请求可正常发起并反向互斥 */
    assert(ecat_transfer_begin(&mgr, &direct_owner, &upg_req, &upg_token) == ECAT_TRANSFER_COMPLETE);
    assert(upg_token != 0);

    /* 直接请求活跃期间，所有新端点发起操作必须被拦截返回 BUSY */
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 3U, 16, 0) == ECAT_TRANSFER_BUSY);
    assert(ecat_transport_endpoint_begin_export(&ep_can, 4U, NULL) == ECAT_TRANSFER_BUSY);

    /* 直接请求取消 释放 */
    assert(ecat_transfer_cancel(&mgr, &direct_owner, upg_token) == ECAT_TRANSFER_COMPLETE);

    /* 释放后新端点可再次正常发起 */
    assert(ecat_transport_endpoint_begin_export(&ep_can, 4U, &log_size) == ECAT_TRANSFER_COMPLETE);
    ecat_transport_endpoint_abort(&ep_can);

    /* ---------------------------------------------------------------------- */
    /* 测试阶段 D: SOES、SSC 与文件流直接接入权威统一传输核心调用链与互斥仲裁   */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试阶段 D: SOES、SSC 与文件流直接接入统一传输核心真实调用链验证 ---\n");

    /* D0: 混合配置集成测试：当注册表中存在启用的异步目标 (目标 2 与目标 5) 时，
     * 纯同步 SOES 适配器与无 gate SSC 适配器必须能够顺利绑定传输核心；
     * FoE 请求同步目标正常完成，请求异步目标在发 ACK 前被拦截且不泄露会话；
     * 其他有异步能力的通道 (如 UART) 仍可正常访问异步目标。
     */
    static const ecat_foe_target_async_ops_t e2e_fw_async_ops = {
        e2e_fw_async_start,
        e2e_fw_async_poll,
        e2e_fw_async_abort,
        e2e_fw_async_abort_session
    };
    /* 注册异步固件目标 2 (FPGA)，用于验证纯同步 SOES/无 gate SSC 拒绝异步固件目标 */
    ecat_target_desc_t fpga_desc;
    memset(&fpga_desc, 0, sizeof(fpga_desc));
    fpga_desc.target_id = 2U;
    strncpy(fpga_desc.name, "fpga_async", sizeof(fpga_desc.name));
    fpga_desc.type = ECAT_TARGET_TYPE_FIRMWARE;
    fpga_desc.permissions = ECAT_PERM_WRITE;
    fpga_desc.max_size = 512U * 1024U;
    fpga_desc.write_unit = 8U;
    fpga_desc.enabled = true;
    fpga_desc.asynchronous = true;
    fpga_desc.driver.io = mcu_desc.driver.io;
    fpga_desc.driver.fw_async_ops = &e2e_fw_async_ops;
    assert(ecat_target_registry_add(&reg, &fpga_desc) == 0);
    assert(ecat_target_registry_add_acl(&reg, 1U, 2U, ECAT_PERM_WRITE) == 0);

    ecat_foe_soes_adapter_t soes_adapter;
    ecat_foe_soes_init(&soes_adapter, &core, 128U);
    /* D0.1: 混合目标共存下，纯同步 SOES 适配器成功绑定 transfer 核心 */
    assert(ecat_foe_soes_bind_transfer(&soes_adapter, &mgr) == 0);
    /* 重复绑定防御拦截 */
    assert(ecat_foe_soes_bind_transfer(&soes_adapter, &mgr) == -1);

    /* D0.2: 纯同步 SOES 请求异步目标 2，在发 ACK 前必须被拦截 (返回 -1)，不激活会话 */
    const char *soes_fpga_fn = "DRIVE_SERVO_FPGA_V1.0.0.0.efw";
    assert(ecat_foe_soes_on_wrq(&soes_adapter, soes_fpga_fn, strlen(soes_fpga_fn)) != 0);
    assert(ecat_transfer_is_active(&mgr) == false);
    /* 重复请求异步目标依然被拒绝，无租约泄漏 */
    assert(ecat_foe_soes_on_wrq(&soes_adapter, soes_fpga_fn, strlen(soes_fpga_fn)) != 0);
    assert(ecat_transfer_is_active(&mgr) == false);

    /* D0.3: 在 FoE 拒绝异步目标的同时，其他有能力的通道 (UART 端点) 仍可正常异步访问目标 5 */
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 5U, 64, 0) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_foe_soes_on_wrq(&soes_adapter, soes_fpga_fn, strlen(soes_fpga_fn)) != 0);
    assert(ecat_transport_endpoint_is_active(&ep_uart));
    ecat_transport_endpoint_abort(&ep_uart);
    assert(ecat_transfer_is_active(&mgr) == false);

    /* D1: SOES 适配器直连统一传输核心功能验证 */
    /* D1.1: ACL 越权拦截 - FoE (通道 1) 未获权写入目标 3 (SII) */
    const char *soes_sii_fn = "DRIVE_SERVO_SII_V1.0.0.0.efw";
    assert(ecat_foe_soes_on_wrq(&soes_adapter, soes_sii_fn, strlen(soes_sii_fn)) != 0);
    assert(ecat_transfer_is_active(&mgr) == false);

    /* D1.2: 合法固件全流程烧录 - FoE 写入目标 1 (MCU) */
    const char *soes_mcu_fn = "DRIVE_SERVO_MCU_V1.0.0.0.efw";
    assert(ecat_foe_soes_on_wrq(&soes_adapter, soes_mcu_fn, strlen(soes_mcu_fn)) == 0);
    assert(ecat_transfer_is_active(&mgr) == true);
    /* 互斥检查: 此时 UART 试图写系统参数必须被仲裁拦截 (BUSY) */
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 3U, 16, 0) == ECAT_TRANSFER_BUSY);

    /* 构造合法的 180 字节固件包 (148 头 + 32 负载) */
    uint8_t fw_payload[32];
    for (size_t i = 0; i < sizeof(fw_payload); i++) fw_payload[i] = (uint8_t)(i + 0x33);
    uint8_t fw_buf[256];
    size_t fw_len = build_e2e_firmware(fw_buf, sizeof(fw_buf), 1U, 0x01000000U, fw_payload, sizeof(fw_payload));
    assert(fw_len == 180U);

    /* 写入第 1 块 (128 字节，全长包) */
    assert(ecat_foe_soes_write_cb(NULL, fw_buf, 128U) == 0U);
    assert(ecat_transfer_is_active(&mgr) == true);
    /* 拒绝异步目标的新 WRQ 时，必须清理本适配器持有的旧会话。 */
    assert(ecat_foe_soes_on_wrq(&soes_adapter, soes_fpga_fn, strlen(soes_fpga_fn)) != 0);
    assert(ecat_transfer_is_active(&mgr) == false);
    assert(ecat_foe_soes_on_wrq(&soes_adapter, soes_mcu_fn, strlen(soes_mcu_fn)) == 0);
    assert(ecat_foe_soes_write_cb(NULL, fw_buf, 128U) == 0U);
    /* 写入第 2 块 (52 字节 < 128U，短包触发 finalize 提交) */
    assert(ecat_foe_soes_write_cb(NULL, fw_buf + 128U, 52U) == 0U);
    /* 校验写入数据一致性与会话干净释放 */
    assert(memcmp(s_flash_mcu, fw_payload, sizeof(fw_payload)) == 0);
    assert(ecat_transfer_is_active(&mgr) == false);

    /* D1.3: 损坏固件包防御与单次精确中止清理 */
    assert(ecat_foe_soes_on_wrq(&soes_adapter, soes_mcu_fn, strlen(soes_mcu_fn)) == 0);
    assert(ecat_transfer_is_active(&mgr) == true);
    uint8_t bad_fw[128];
    memcpy(bad_fw, fw_buf, sizeof(bad_fw));
    bad_fw[0] = 0x00; /* 破坏魔数 */
    assert(ecat_foe_soes_write_cb(NULL, bad_fw, sizeof(bad_fw)) != 0U);
    /* 发生错误后必须自动释放租约并重置状态，无泄漏 */
    assert(ecat_transfer_is_active(&mgr) == false);

    /* D1.4: 重复 WRQ 序列与断链自愈 */
    assert(ecat_foe_soes_on_wrq(&soes_adapter, soes_mcu_fn, strlen(soes_mcu_fn)) == 0);
    assert(ecat_transfer_is_active(&mgr) == true);
    /* 在未写入数据时再次发起新的 WRQ，旧会话安全自愈重置并重新建立 */
    assert(ecat_foe_soes_on_wrq(&soes_adapter, soes_mcu_fn, strlen(soes_mcu_fn)) == 0);
    assert(ecat_transfer_is_active(&mgr) == true);
    ecat_foe_transport_abort(&soes_adapter.transport);
    assert(ecat_transfer_is_active(&mgr) == false);

    /* ---------------------------------------------------------------------- */
    /* D2: SSC 适配器直连统一传输核心 (ecat_foe_ssc_bind_transfer)             */
    /* ---------------------------------------------------------------------- */
    ecat_foe_ssc_adapter_t ssc_adapter;
    ecat_foe_ssc_init(&ssc_adapter, &core, 128U);
    /* D2.0: 纯同步 SSC 适配器成功绑定混合目标注册表 */
    assert(ecat_foe_ssc_bind_transfer(&ssc_adapter, &mgr) == 0);
    assert(ecat_foe_ssc_bind_transfer(&ssc_adapter, &mgr) == -1);

    /* D2.0.1: 纯同步 SSC 请求异步目标 2，在发 ACK 前返回 ECAT_FOE_SSC_ERR_ILLEGAL 拦截 */
    assert(ecat_foe_ssc_on_write(&ssc_adapter, soes_fpga_fn, (uint16_t)strlen(soes_fpga_fn), 0U) == ECAT_FOE_SSC_ERR_ILLEGAL);
    assert(ecat_transfer_is_active(&mgr) == false);
    /* 重复请求异步目标防御，无租约泄漏 */
    assert(ecat_foe_ssc_on_write(&ssc_adapter, soes_fpga_fn, (uint16_t)strlen(soes_fpga_fn), 0U) == ECAT_FOE_SSC_ERR_ILLEGAL);
    assert(ecat_transfer_is_active(&mgr) == false);

    /* D2.1: SSC 同步模式固件写入与全流程校验 */
    assert(ecat_foe_ssc_on_write(&ssc_adapter, soes_mcu_fn, (uint16_t)strlen(soes_mcu_fn), 0U) == 0);
    assert(ecat_transfer_is_active(&mgr) == true);
    /* 跨通道互斥检查: CAN 试图导出被阻断 (BUSY) */
    assert(ecat_transport_endpoint_begin_export(&ep_can, 4U, NULL) == ECAT_TRANSFER_BUSY);

    /* 改变 payload 数据以区分来源 */
    for (size_t i = 0; i < sizeof(fw_payload); i++) fw_payload[i] = (uint8_t)(i + 0x88);
    fw_len = build_e2e_firmware(fw_buf, sizeof(fw_buf), 1U, 0x01000000U, fw_payload, sizeof(fw_payload));
    assert(fw_len == 180U);

    /* 递交第 1 块 (128 字节) */
    assert(ecat_foe_ssc_on_data(&ssc_adapter, fw_buf, 128U) == 0);
    assert(ecat_transfer_is_active(&mgr) == true);
    /* 递交尾包 (52 字节短包结束) */
    assert(ecat_foe_ssc_on_data(&ssc_adapter, fw_buf + 128U, 52U) == 0);
    assert(memcmp(s_flash_mcu, fw_payload, sizeof(fw_payload)) == 0);
    assert(ecat_transfer_is_active(&mgr) == false);

    /* ---------------------------------------------------------------------- */
    /* D3: 文件流适配器直连统一传输核心 (ecat_transfer_file_init)      */
    /* ---------------------------------------------------------------------- */
    ecat_transfer_file_t file_tr;
    uint8_t file_chunk_buf[64];
    assert(ecat_transfer_file_init(&file_tr, &mgr, file_chunk_buf, sizeof(file_chunk_buf)) == 0);
    assert(ecat_transfer_file_init(NULL, &mgr, file_chunk_buf, sizeof(file_chunk_buf)) == -1);

    /* D3.1: ACL 越权拦截 - FILE 通道 (通道 2) 初始未被授权写 MCU 固件 (目标 1) */
    ecat_transfer_request_t upg_mcu_req = { .transport_id = 2U, .target_id = 1U, .direction = ECAT_TRANSFER_DIR_IMPORT, .fw_version = 0x01000000U };
    FILE *tmp_fp = tmpfile();
    assert(tmp_fp != NULL);
    assert(fwrite(fw_buf, 1U, fw_len, tmp_fp) == fw_len);
    rewind(tmp_fp);

    /* Invalid direction is rejected without consuming the stream. */
    ecat_transfer_request_t invalid_file_req = upg_mcu_req;
    invalid_file_req.direction = ECAT_TRANSFER_DIR_EXPORT;
    assert(ecat_transfer_file_begin(&file_tr, tmp_fp, &invalid_file_req) == ECAT_TRANSFER_INVALID);
    assert(ftell(tmp_fp) == 0);

    /* 未授权写目标 1，begin 必须返回 DENIED 且租约未激活 */
    assert(ecat_transfer_file_begin(&file_tr, tmp_fp, &upg_mcu_req) == ECAT_TRANSFER_DENIED);
    assert(ecat_transfer_is_active(&mgr) == false);
    /* The adapter honors the caller's channel ID rather than hard-coding 2. */
    ecat_transfer_request_t allowed_file_req = upg_mcu_req;
    allowed_file_req.transport_id = 1U;
    assert(ecat_transfer_file_begin(&file_tr, tmp_fp, &allowed_file_req) == ECAT_TRANSFER_COMPLETE);
    assert(ftell(tmp_fp) == 0);
    assert(ecat_transfer_file_cancel(&file_tr) == ECAT_TRANSFER_COMPLETE);
    fclose(tmp_fp);

    /* D3.2: 授权后流式写入与完成校验 */
    /* 为通道 2 授予写目标 1 权限 */
    assert(ecat_target_registry_add_acl(&reg, 2U, 1U, ECAT_PERM_WRITE) == 0);

    /* 重新准备固件负载数据 */
    for (size_t i = 0; i < sizeof(fw_payload); i++) fw_payload[i] = (uint8_t)(i + 0xEE);
    fw_len = build_e2e_firmware(fw_buf, sizeof(fw_buf), 1U, 0x01000000U, fw_payload, sizeof(fw_payload));
    tmp_fp = tmpfile();
    assert(tmp_fp != NULL);
    assert(fwrite(fw_buf, 1U, fw_len, tmp_fp) == fw_len);
    rewind(tmp_fp);

    assert(ecat_transfer_file_begin(&file_tr, tmp_fp, &upg_mcu_req) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_is_active(&mgr) == true);
    /* 此时 FoE 端点被互斥阻断 */
    assert(ecat_transport_endpoint_begin_import(&ep_foe, 1U, 1024, 0x01000000U) == ECAT_TRANSFER_BUSY);

    /* 循环 step 流式写入直到 EOF */
    ecat_transfer_result_t f_step_res = ECAT_TRANSFER_PENDING;
    for (unsigned s = 0; f_step_res == ECAT_TRANSFER_PENDING && s < 32U; s++) {
        f_step_res = ecat_transfer_file_step(&file_tr);
    }
    assert(f_step_res == ECAT_TRANSFER_COMPLETE);
    assert(memcmp(s_flash_mcu, fw_payload, sizeof(fw_payload)) == 0);
    assert(ecat_transfer_is_active(&mgr) == false);
    fclose(tmp_fp);

    /* D3.3: 文件流中途 cancel 安全取消与租约释放 */
    tmp_fp = tmpfile();
    assert(tmp_fp != NULL);
    assert(fwrite(fw_buf, 1U, fw_len, tmp_fp) == fw_len);
    rewind(tmp_fp);

    assert(ecat_transfer_file_begin(&file_tr, tmp_fp, &upg_mcu_req) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_is_active(&mgr) == true);
    /* 写入第 1 块 */
    assert(ecat_transfer_file_step(&file_tr) == ECAT_TRANSFER_PENDING);
    /* 调用 cancel 取消传输 */
    assert(ecat_transfer_file_cancel(&file_tr) == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_is_active(&mgr) == false);
    /* 释放后 UART 端点可立即发起新操作 */
    assert(ecat_transport_endpoint_begin_export(&ep_uart, 3U, &param_export_size) == ECAT_TRANSFER_COMPLETE);
    ecat_transport_endpoint_abort(&ep_uart);
    fclose(tmp_fp);

    /* D3.4: 固件数字签名 (Ed25519) 流式核验与失败防御 */
    test_e2e_sig_verifier_t sig_verifier;
    memset(&sig_verifier, 0, sizeof(sig_verifier));
    core.cfg.signature_verify_enable = true;
    core.cfg.signature_verifier.begin = e2e_sig_begin;
    core.cfg.signature_verifier.update = e2e_sig_update;
    core.cfg.signature_verifier.finish = e2e_sig_finish;
    core.cfg.signature_verifier.abort = e2e_sig_abort;
    core.cfg.signature_verifier.user_ctx = &sig_verifier;

    /* 路径 1: 签名验证通过全流程 */
    tmp_fp = tmpfile();
    assert(tmp_fp != NULL);
    assert(fwrite(fw_buf, 1U, fw_len, tmp_fp) == fw_len);
    rewind(tmp_fp);

    assert(ecat_transfer_file_begin(&file_tr, tmp_fp, &upg_mcu_req) == ECAT_TRANSFER_COMPLETE);
    f_step_res = ECAT_TRANSFER_PENDING;
    for (unsigned s = 0; f_step_res == ECAT_TRANSFER_PENDING && s < 32U; s++) {
        f_step_res = ecat_transfer_file_step(&file_tr);
    }
    assert(f_step_res == ECAT_TRANSFER_COMPLETE);
    assert(sig_verifier.begin_calls == 1U);
    assert(sig_verifier.update_calls > 0U);
    assert(sig_verifier.finish_calls == 1U);
    assert(sig_verifier.abort_calls == 0U);
    assert(ecat_transfer_is_active(&mgr) == false);
    fclose(tmp_fp);

    /* 路径 2: 签名验证失败拦截与安全回滚 */
    sig_verifier.fail_finish = true;
    tmp_fp = tmpfile();
    assert(tmp_fp != NULL);
    assert(fwrite(fw_buf, 1U, fw_len, tmp_fp) == fw_len);
    rewind(tmp_fp);

    assert(ecat_transfer_file_begin(&file_tr, tmp_fp, &upg_mcu_req) == ECAT_TRANSFER_COMPLETE);
    f_step_res = ECAT_TRANSFER_PENDING;
    for (unsigned s = 0; f_step_res == ECAT_TRANSFER_PENDING && s < 32U; s++) {
        f_step_res = ecat_transfer_file_step(&file_tr);
    }
    assert(f_step_res == ECAT_TRANSFER_FAILED);
    assert(sig_verifier.abort_calls > 0U);
    assert(ecat_transfer_is_active(&mgr) == false);
    fclose(tmp_fp);
    core.cfg.signature_verify_enable = false;
    sig_verifier.fail_finish = false;

    /* ---------------------------------------------------------------------- */
    /* D3.5: 异步 begin PENDING 生命周期、step 推进与防租约泄漏验证            */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试 D3.5: 文件流适配器异步 begin PENDING 生命周期与防租约泄漏 ---\n");
    ecat_transfer_request_t upg_async_data_req = { .transport_id = 2U, .target_id = 5U, .direction = ECAT_TRANSFER_DIR_IMPORT };
    uint8_t async_file_data[128];
    memset(async_file_data, 0x33, sizeof(async_file_data));
    tmp_fp = tmpfile();
    assert(tmp_fp != NULL);
    assert(fwrite(async_file_data, 1U, sizeof(async_file_data), tmp_fp) == sizeof(async_file_data));
    rewind(tmp_fp);

    /* D3.5.1: 异步驱动 INIT 返回 PENDING 时，begin 必须返回 PENDING 并正确激活 active/pending */
    s_e2e_async_res = ECAT_ASYNC_OP_PENDING;
    assert(ecat_transfer_file_begin(&file_tr, tmp_fp, &upg_async_data_req) == ECAT_TRANSFER_PENDING);
    assert(file_tr.active == true);
    assert(file_tr.pending == true);
    assert(ecat_transfer_is_active(&mgr) == true);
    assert(ecat_transfer_is_pending(&mgr) == true);
    /* 互斥阻断验证: pending begin 期间其它通道被 BUSY 拦截 */
    assert(ecat_transport_endpoint_begin_import(&ep_foe, 1U, 1024, 0x01000000U) == ECAT_TRANSFER_BUSY);

    /* D3.5.2: 在 pending begin 期间调用 cancel，必须安全释放租约，杜绝租约泄漏 */
    assert(ecat_transfer_file_cancel(&file_tr) == ECAT_TRANSFER_COMPLETE);
    assert(file_tr.active == false);
    assert(file_tr.pending == false);
    assert(ecat_transfer_is_active(&mgr) == false);
    /* 释放后冲突消除，其它通道可立即占用 */
    assert(ecat_transport_endpoint_begin_import(&ep_foe, 1U, 1024, 0x01000000U) == ECAT_TRANSFER_COMPLETE);
    ecat_transport_endpoint_abort(&ep_foe);
    assert(ecat_transfer_is_active(&mgr) == false);

    /* D3.5.3: 重新发起 begin PENDING，并通过 step 推进就绪与数据写入 */
    rewind(tmp_fp);
    s_e2e_async_res = ECAT_ASYNC_OP_PENDING;
    assert(ecat_transfer_file_begin(&file_tr, tmp_fp, &upg_async_data_req) == ECAT_TRANSFER_PENDING);
    assert(file_tr.active == true);
    assert(file_tr.pending == true);

    /* step 推进: 驱动仍 pending 时 step 返回 PENDING */
    assert(ecat_transfer_file_step(&file_tr) == ECAT_TRANSFER_PENDING);
    assert(file_tr.pending == true);

    /* 驱动完成 INIT/INVALIDATE */
    s_e2e_async_res = ECAT_ASYNC_OP_COMPLETE;
    assert(ecat_transfer_file_step(&file_tr) == ECAT_TRANSFER_PENDING);
    assert(file_tr.pending == false); /* begin 轮询完成，就绪态 */

    /* 继续 step 循环直到文件传输完成 */
    f_step_res = ECAT_TRANSFER_PENDING;
    for (unsigned s = 0; f_step_res == ECAT_TRANSFER_PENDING && s < 32U; s++) {
        f_step_res = ecat_transfer_file_step(&file_tr);
    }
    assert(f_step_res == ECAT_TRANSFER_COMPLETE);
    assert(ecat_transfer_is_active(&mgr) == false);
    fclose(tmp_fp);

    /* ---------------------------------------------------------------------- */
    /* D3.6: FoE 适配器直连统一核心完整流程、abort 释放与会话互斥消除验证      */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试 D3.6: FoE 适配器直连统一核心完整流程与 abort 释放 ---\n");
    ecat_foe_transport_t foe_tr;
    assert(ecat_foe_transport_init_transfer(&foe_tr, &mgr) == 0);
    const char *wrq_name = "DRIVE_SERVO_MCU_V1.0.0.0.efw";

    /* D3.6.1: FoE begin 成功接收，会话激活并排他独占 */
    assert(ecat_foe_transport_begin(&foe_tr, wrq_name, strlen(wrq_name)) == 0);
    assert(ecat_foe_transport_owns(&foe_tr) == true);
    assert(ecat_transfer_is_active(&mgr) == true);

    /* 互斥阻断: 其它端点此时无法 begin */
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 5U, 64, 0) == ECAT_TRANSFER_BUSY);

    /* D3.6.2: 中途调用 abort，安全取消会话并释放租约 */
    ecat_foe_transport_abort(&foe_tr);
    assert(foe_tr.pending == false);
    assert(ecat_foe_transport_owns(&foe_tr) == false);
    assert(ecat_transfer_is_active(&mgr) == false);

    /* 释放后冲突消除，其它通道可立即占用 */
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 5U, 64, 0) == ECAT_TRANSFER_COMPLETE);
    ecat_transport_endpoint_abort(&ep_uart);
    assert(ecat_transfer_is_active(&mgr) == false);

    /* D3.6.3: 重新发起 FoE begin，分块传输完整固件包并提交成功 */
    assert(ecat_foe_transport_begin(&foe_tr, wrq_name, strlen(wrq_name)) == 0);
    assert(ecat_foe_transport_owns(&foe_tr) == true);
    /* 写入第 1 块 100 字节 (more_data = true) */
    assert(ecat_foe_transport_start(&foe_tr, fw_buf, 100U, true) == ECAT_UPGRADE_ASYNC_COMPLETE);
    assert(foe_tr.committed == false);
    /* 写入第 2 块 80 字节 (总计 180 字节，more_data = false，自动触发 finish 提交) */
    assert(ecat_foe_transport_start(&foe_tr, fw_buf + 100U, 80U, false) == ECAT_UPGRADE_ASYNC_COMPLETE);
    assert(foe_tr.committed == true);
    assert(ecat_transfer_is_active(&mgr) == false);

    /* ---------------------------------------------------------------------- */
    /* D3.7: 真实 SSC 协议回调验证异步固件核心握手时序、超时重发与独立暂存清零  */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试 D3.7: SSC 适配器契约回调验证异步固件 BUSY 握手、超时与重复 WRQ ---\n");
    ecat_foe_core_async_t core_async;
    uint8_t core_async_pkt[256];
    uint8_t core_async_op[128];
    assert(ecat_foe_core_async_init(&core_async, &core,
                                    core_async_pkt, sizeof(core_async_pkt),
                                    core_async_op, sizeof(core_async_op)) == 0);
    assert(ecat_transfer_init_async_firmware(&mgr, &core_async) == 0);

    ecat_foe_ssc_adapter_t ssc_adp;
    ecat_foe_ssc_async_t ssc_gate;
    uint8_t ssc_storage[256];
    ecat_foe_ssc_init_async(&ssc_adp, &core, &ssc_gate);
    assert(ecat_foe_ssc_async_init(&ssc_gate, ecat_foe_transport_async_ops(), &ssc_adp.transport,
                                   ssc_storage, sizeof(ssc_storage), 100U, 0x8007U,
                                   ECAT_FOE_SSC_ERR_PACKETNO, ECAT_FOE_SSC_ERR_PROGERROR) == 0);
    assert(ecat_foe_ssc_bind_transfer(&ssc_adp, &mgr) == 0);

    const char *wrq_fpga = "DRIVE_SERVO_FPGA_V1.0.0.0.efw";
    uint8_t fpga_payload[32];
    for (size_t i = 0; i < sizeof(fpga_payload); i++) {
        fpga_payload[i] = (uint8_t)(0xA0U + i);
    }
    uint8_t fpga_fw[256];
    size_t fpga_fw_len = build_e2e_firmware(
        fpga_fw, sizeof(fpga_fw), 2U, 0x01000000U, fpga_payload, sizeof(fpga_payload));
    assert(fpga_fw_len == 180U);
    /* 首包 164 字节 (148 字节 EFW 头 + 前 16 字节负载)，次包 16 字节负载 */
    const uint8_t *fpga_pkt1 = fpga_fw;
    const size_t fpga_pkt1_len = 164U;
    const uint8_t *fpga_pkt2 = fpga_fw + fpga_pkt1_len;
    const size_t fpga_pkt2_len = 16U;
    uint32_t ssc_tick = 1000U;

    /* D3.7.1: 异步挂起下重复 WRQ，验证前次会话被安全 abort 并重新获取新租约 */
    e2e_fw_hw_reset();
    s_e2e_fw_async_res = ECAT_FOE_TARGET_ASYNC_PENDING;
    assert(ecat_foe_ssc_on_write(&ssc_adp, wrq_fpga, strlen(wrq_fpga), 0) == 0U);
    assert(ssc_adp.session_active == true);
    assert(ecat_transfer_is_active(&mgr) == true);
    uint64_t tok1 = ssc_adp.transport.token;
    assert(tok1 != 0U);
    assert(ecat_foe_ssc_on_data_following(&ssc_adp, fpga_pkt1, (uint16_t)fpga_pkt1_len, true, ssc_tick++) == 0x8007U);
    assert(ssc_gate.pending == true);
    assert(ssc_adp.transport.pending == true);
    assert(s_e2e_hw_state.hardware_active == true);

    /* 重复 WRQ 到达: 必须安全 abort 前次底层硬件与会话，并重新 begin 建立新租约 */
    assert(ecat_foe_ssc_on_write(&ssc_adp, wrq_fpga, strlen(wrq_fpga), 0) == 0U);
    assert(s_e2e_fw_async_aborts == 1U);
    assert(s_e2e_hw_state.hardware_active == false);
    assert(ssc_adp.transport.token != tok1);
    assert(ecat_transfer_is_active(&mgr) == true);

    /* D3.7.2: 挂起期间调用 abort，验证会话彻底释放且无租约泄漏 */
    assert(ecat_foe_ssc_on_data_following(&ssc_adp, fpga_pkt1, (uint16_t)fpga_pkt1_len, true, ssc_tick++) == 0x8007U);
    ecat_foe_ssc_on_abort(&ssc_adp, 0);
    assert(s_e2e_fw_async_aborts == 2U);
    assert(s_e2e_hw_state.hardware_active == false);
    assert(ssc_adp.session_active == false);
    assert(ssc_adp.transport.pending == false);
    assert(ssc_adp.transport.begin_pending == false);
    assert(ecat_foe_transport_owns(&ssc_adp.transport) == false);
    assert(ecat_transfer_is_active(&mgr) == false);

    /* 租约释放后，其它端点可无障碍占用 */
    s_e2e_async_res = ECAT_ASYNC_OP_PENDING;
    assert(ecat_transport_endpoint_begin_import(&ep_uart, 5U, 64, 0) == ECAT_TRANSFER_PENDING);
    assert(ecat_transfer_is_active(&mgr) == true);
    ecat_transport_endpoint_abort(&ep_uart);
    assert(ecat_transfer_is_active(&mgr) == false);

    /* D3.7.3: 首包到达返回 BUSY (0x8007) -> 延迟重试 -> 轮询推进 -> 末包 finish 提交 */
    e2e_fw_hw_reset();
    memset(s_flash_fpga, 0, sizeof(s_flash_fpga));
    s_e2e_fw_async_res = ECAT_FOE_TARGET_ASYNC_PENDING;
    assert(ecat_foe_ssc_on_write(&ssc_adp, wrq_fpga, strlen(wrq_fpga), 0) == 0U);

    /* 首包到达: 触发头部解析与异步驱动启动，向协议栈返回 BUSY (0x8007) 且包暂存于 ssc_storage */
    uint16_t d1_code = ecat_foe_ssc_on_data_following(&ssc_adp, fpga_pkt1, (uint16_t)fpga_pkt1_len, true, ssc_tick++);
    assert(d1_code == 0x8007U);
    assert(ssc_gate.pending == true);
    assert(memcmp(ssc_storage, fpga_pkt1, fpga_pkt1_len) == 0);

    /* 主站重发首包 (数据一致): 门控依然拦截并回复 BUSY */
    assert(ecat_foe_ssc_on_data_following(&ssc_adp, fpga_pkt1, (uint16_t)fpga_pkt1_len, true, ssc_tick++) == 0x8007U);

    /* 底层固件异步驱动就绪完成 */
    s_e2e_fw_async_res = ECAT_FOE_TARGET_ASYNC_COMPLETE;

    /* 从站主循环轮询: 推进 INIT -> INVALIDATE -> ERASE -> WRITE 落盘 */
    uint16_t main_res = ecat_foe_ssc_on_main_loop(&ssc_adp, ssc_tick++);
    assert(main_res == 0U);
    assert(ssc_gate.completed == true);
    assert(ssc_gate.pending == false);
    assert(s_e2e_hw_state.hardware_active == false);
    assert(s_e2e_hw_state.received_len == 16U);
    assert(memcmp(s_e2e_hw_state.received_payload, fpga_payload, 16U) == 0);

    /* 主站再次重传首包: 门控完成重试握手并返回 0 (ACK)，清零暂存区 */
    assert(ecat_foe_ssc_on_data_following(&ssc_adp, fpga_pkt1, (uint16_t)fpga_pkt1_len, true, ssc_tick++) == 0U);
    for (size_t i = 0; i < fpga_pkt1_len; i++) {
        assert(ssc_storage[i] == 0U);
    }

    /* 发送第 2 包 (末包，16 字节，more_data = false): 触发回读核验、FINALIZE 提交并关闭会话 */
    assert(ecat_foe_ssc_on_data_following(&ssc_adp, fpga_pkt2, (uint16_t)fpga_pkt2_len, false, ssc_tick++) == 0U);
    assert(ssc_adp.transport.committed == true);
    assert(s_e2e_hw_state.hardware_active == false);
    assert(memcmp(s_flash_fpga, fpga_payload, sizeof(fpga_payload)) == 0);
    assert(ecat_transfer_is_active(&mgr) == false);

    /* ---------------------------------------------------------------------- */
    /* D3.7.4: SSC 适配器契约回调下共享缓存超时重试回归验证                    */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试 D3.7.4: 共享缓存下异步首包超时重发错误码验证 ---\n");
    e2e_fw_hw_reset();
    s_e2e_fw_async_res = ECAT_FOE_TARGET_ASYNC_PENDING;
    assert(ecat_foe_ssc_on_write(&ssc_adp, wrq_fpga, strlen(wrq_fpga), 0) == 0U);

    uint16_t b_res = ecat_foe_ssc_on_data_following(&ssc_adp, fpga_pkt1, (uint16_t)fpga_pkt1_len, true, ssc_tick++);
    assert(b_res == 0x8007U);
    assert(ssc_gate.pending == true);
    assert(memcmp(ssc_storage, fpga_pkt1, fpga_pkt1_len) == 0);

    /* 模拟底层驱动持续 PENDING，时间流逝超过 timeout_ticks (100U) */
    ssc_tick += 150U;

    /* 主站因超时重发同一个非零首包: 匹配成功并返回 ECAT_FOE_SSC_ERR_PROGERROR (0x800B) */
    uint16_t timeout_retry_code = ecat_foe_ssc_on_data_following(&ssc_adp, fpga_pkt1, (uint16_t)fpga_pkt1_len, true, ssc_tick++);
    assert(timeout_retry_code == ECAT_FOE_SSC_ERR_PROGERROR);
    assert(s_e2e_hw_state.hardware_active == false);
    assert(ssc_gate.pending == false);
    assert(ssc_gate.completed == false);
    for (size_t i = 0; i < fpga_pkt1_len; i++) {
        assert(ssc_storage[i] == 0U);
    }
    assert(ecat_transfer_is_active(&mgr) == false);

    /* ---------------------------------------------------------------------- */
    /* D3.7.5: 异包重发测试 (数据 payload 被篡改或错包)                        */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试 D3.7.5: 超时后异包重发严格返回 PACKETNO (0x8005) 防御 ---\n");
    e2e_fw_hw_reset();
    s_e2e_fw_async_res = ECAT_FOE_TARGET_ASYNC_PENDING;
    assert(ecat_foe_ssc_on_write(&ssc_adp, wrq_fpga, strlen(wrq_fpga), 0) == 0U);
    assert(ecat_foe_ssc_on_data_following(&ssc_adp, fpga_pkt1, (uint16_t)fpga_pkt1_len, true, ssc_tick++) == 0x8007U);
    ssc_tick += 150U; /* 超时 */

    uint8_t corrupt_pkt[164];
    memcpy(corrupt_pkt, fpga_pkt1, sizeof(corrupt_pkt));
    corrupt_pkt[0] = 0xFF;
    uint16_t corrupt_res = ecat_foe_ssc_on_data_following(&ssc_adp, corrupt_pkt, sizeof(corrupt_pkt), true, ssc_tick++);
    assert(corrupt_res == ECAT_FOE_SSC_ERR_PACKETNO);
    assert(s_e2e_hw_state.hardware_active == false);
    assert(ssc_adp.session_active == false);
    assert(ssc_gate.pending == false);
    assert(ssc_gate.completed == false);
    for (size_t i = 0; i < fpga_pkt1_len; i++) {
        assert(ssc_storage[i] == 0U);
    }
    assert(ecat_transfer_is_active(&mgr) == false);

    /* ---------------------------------------------------------------------- */
    /* D3.7.6: 主循环轮询触发超时后重试到达链路验证                           */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试 D3.7.6: 主循环轮询先发现超时后重试握手闭环验证 ---\n");
    e2e_fw_hw_reset();
    s_e2e_fw_async_res = ECAT_FOE_TARGET_ASYNC_PENDING;
    assert(ecat_foe_ssc_on_write(&ssc_adp, wrq_fpga, strlen(wrq_fpga), 0) == 0U);
    assert(ecat_foe_ssc_on_data_following(&ssc_adp, fpga_pkt1, (uint16_t)fpga_pkt1_len, true, ssc_tick++) == 0x8007U);
    ssc_tick += 150U; /* 超时 */

    assert(ecat_foe_ssc_on_main_loop(&ssc_adp, ssc_tick++) == ECAT_FOE_SSC_ERR_PROGERROR);
    assert(s_e2e_hw_state.hardware_active == false);
    assert(ssc_gate.completed == true);
    assert(ssc_gate.operation_failed == true);
    assert(memcmp(ssc_storage, fpga_pkt1, fpga_pkt1_len) == 0);

    assert(ecat_foe_ssc_on_data_following(&ssc_adp, fpga_pkt1, (uint16_t)fpga_pkt1_len, true, ssc_tick++) == ECAT_FOE_SSC_ERR_PROGERROR);
    assert(ssc_gate.pending == false);
    assert(ssc_gate.completed == false);
    for (size_t i = 0; i < fpga_pkt1_len; i++) {
        assert(ssc_storage[i] == 0U);
    }
    assert(ecat_transfer_is_active(&mgr) == false);

    /* ---------------------------------------------------------------------- */
    /* D3.7.7: 独立独占暂存区 (非共享) 行为对比与 abort 敏感数据清零验证      */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试 D3.7.7: 独立独占暂存区对比与 abort 安全清零验证 ---\n");
    ecat_foe_ssc_adapter_t ssc_adp_indep;
    ecat_foe_ssc_async_t ssc_gate_indep;
    uint8_t gate_storage_indep[128];
    uint8_t indep_stage_buf[128];
    ecat_foe_ssc_init_async(&ssc_adp_indep, &core, &ssc_gate_indep);
    assert(ecat_foe_ssc_async_init(&ssc_gate_indep, ecat_foe_transport_async_ops(), &ssc_adp_indep.transport,
                                   gate_storage_indep, sizeof(gate_storage_indep), 100U, 0x8007U,
                                   ECAT_FOE_SSC_ERR_PACKETNO, ECAT_FOE_SSC_ERR_PROGERROR) == 0);
    assert(ecat_foe_ssc_bind_transfer(&ssc_adp_indep, &mgr) == 0);
    /* 显式配置独立独占暂存区 (覆盖自动复用，staged_shared 为 false) */
    assert(ecat_foe_transport_set_staged_buffer(&ssc_adp_indep.transport, indep_stage_buf, sizeof(indep_stage_buf)) == 0);
    assert(ssc_adp_indep.transport.staged_shared == false);

    /* 在已授权异步 DATA 目标 5 上进入 begin_pending，验证独立暂存区深拷贝与 abort 清零 */
    assert(ecat_target_registry_add_acl(&reg, 1U, 5U, ECAT_PERM_WRITE) == 0);
    ecat_transfer_request_t indep_req = {
        .transport_id = 1U,
        .target_id = 5U,
        .direction = ECAT_TRANSFER_DIR_IMPORT,
        .expected_size = 32U
    };
    s_e2e_async_res = ECAT_ASYNC_OP_PENDING;
    assert(ecat_transfer_begin(&mgr, &ssc_adp_indep.transport, &indep_req, &ssc_adp_indep.transport.token) == ECAT_TRANSFER_PENDING);
    ssc_adp_indep.session_active = true;
    ssc_adp_indep.transport.pending = true;
    ssc_adp_indep.transport.begin_pending = true;

    uint8_t non_zero_first_pkt[32];
    for (size_t i = 0; i < sizeof(non_zero_first_pkt); i++) {
        non_zero_first_pkt[i] = (uint8_t)(i + 0x5A);
    }
    assert(ecat_foe_ssc_on_data_following(&ssc_adp_indep, non_zero_first_pkt, sizeof(non_zero_first_pkt), true, ssc_tick++) == 0x8007U);
    assert(memcmp(indep_stage_buf, non_zero_first_pkt, sizeof(non_zero_first_pkt)) == 0);

    ecat_foe_ssc_on_abort(&ssc_adp_indep, 0U);
    for (size_t i = 0; i < sizeof(non_zero_first_pkt); i++) {
        assert(indep_stage_buf[i] == 0U);
    }
    assert(ecat_transfer_is_active(&mgr) == false);

    /* ---------------------------------------------------------------------- */
    /* D3.7.8: 底层驱动异步写入失败场景生命周期与硬件停止验证                 */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试 D3.7.8: 底层驱动异步写入失败与 abort 硬件停止安全验证 ---\n");
    e2e_fw_hw_reset();
    s_e2e_fw_async_res = ECAT_FOE_TARGET_ASYNC_PENDING;
    assert(ecat_foe_ssc_on_write(&ssc_adp, wrq_fpga, strlen(wrq_fpga), 0) == 0U);
    assert(ssc_adp.transport.begin_pending == false);

    uint16_t f_code = ecat_foe_ssc_on_data_following(&ssc_adp, fpga_pkt1, (uint16_t)fpga_pkt1_len, true, ssc_tick++);
    assert(f_code == 0x8007U);
    assert(ssc_gate.pending == true);
    assert(s_e2e_hw_state.hardware_active == true);
    assert(memcmp(ssc_storage, fpga_pkt1, fpga_pkt1_len) == 0);

    /* 模拟底层固件异步驱动执行失败 */
    s_e2e_fw_async_res = ECAT_FOE_TARGET_ASYNC_FAILED;
    uint16_t f_poll = ecat_foe_ssc_on_main_loop(&ssc_adp, ssc_tick++);
    assert(f_poll == ECAT_FOE_SSC_ERR_PROGERROR);
    assert(s_e2e_hw_state.hardware_active == false);
    assert(ssc_gate.completed == true);
    assert(ssc_gate.operation_failed == true);
    assert(memcmp(ssc_storage, fpga_pkt1, fpga_pkt1_len) == 0);

    assert(ecat_foe_ssc_on_data_following(&ssc_adp, fpga_pkt1, (uint16_t)fpga_pkt1_len, true, ssc_tick++) == ECAT_FOE_SSC_ERR_PROGERROR);
    assert(ssc_gate.pending == false);
    assert(ssc_gate.completed == false);
    for (size_t i = 0; i < fpga_pkt1_len; i++) {
        assert(ssc_storage[i] == 0U);
    }
    assert(ecat_transfer_is_active(&mgr) == false);

    /* ---------------------------------------------------------------------- */
    /* D3.7.9: 数据阶段挂起中途取消与重复 WRQ 硬件停止与清零验证               */
    /* ---------------------------------------------------------------------- */
    printf("--- 测试 D3.7.9: 数据阶段挂起中途取消与重复 WRQ 硬件停止与清零验证 ---\n");
    e2e_fw_hw_reset();
    s_e2e_fw_async_res = ECAT_FOE_TARGET_ASYNC_PENDING;
    assert(ecat_foe_ssc_on_write(&ssc_adp, wrq_fpga, strlen(wrq_fpga), 0) == 0U);
    assert(ecat_foe_ssc_on_data_following(&ssc_adp, fpga_pkt1, (uint16_t)fpga_pkt1_len, true, ssc_tick++) == 0x8007U);
    assert(s_e2e_hw_state.hardware_active == true);

    ecat_foe_ssc_on_abort(&ssc_adp, 0U);
    assert(s_e2e_hw_state.hardware_active == false);
    assert(ssc_gate.pending == false);
    for (size_t i = 0; i < fpga_pkt1_len; i++) {
        assert(ssc_storage[i] == 0U);
    }
    assert(ecat_transfer_is_active(&mgr) == false);

    /* 场景 2: 再次开启会话，在数据挂起期间中途收到新的 WRQ 打断 */
    s_e2e_fw_async_res = ECAT_FOE_TARGET_ASYNC_PENDING;
    assert(ecat_foe_ssc_on_write(&ssc_adp, wrq_fpga, strlen(wrq_fpga), 0) == 0U);
    assert(ecat_foe_ssc_on_data_following(&ssc_adp, fpga_pkt1, (uint16_t)fpga_pkt1_len, true, ssc_tick++) == 0x8007U);
    assert(s_e2e_hw_state.hardware_active == true);

    s_e2e_fw_async_res = ECAT_FOE_TARGET_ASYNC_COMPLETE;
    assert(ecat_foe_ssc_on_write(&ssc_adp, wrq_fpga, strlen(wrq_fpga), 0) == 0U);
    assert(s_e2e_hw_state.hardware_active == false);
    assert(ssc_adp.session_active == true);
    for (size_t i = 0; i < fpga_pkt1_len; i++) {
        assert(ssc_storage[i] == 0U);
    }
    ecat_foe_ssc_on_abort(&ssc_adp, 0U);
    assert(ecat_transfer_is_active(&mgr) == false);

    /* D3.8: 多实例 SOES 适配器活跃会话隔离 (防 init / 异实例被拒 WRQ 篡改 s_active_adapter) */
    printf("--- 测试 D3.8: 多实例 SOES 适配器活跃会话与 DATA 回调路由隔离验证 ---\n");
    {
        ecat_foe_soes_adapter_t peer_soes;
        ecat_target_registry_t peer_reg;
        ecat_foe_core_t peer_core;
        ecat_transfer_t peer_xfer;
        assert(ecat_target_registry_init(&peer_reg) == 0);
        assert(ecat_target_registry_add(&peer_reg, &mcu_desc) == 0);
        ecat_foe_core_cfg_t peer_cfg = core_cfg;
        peer_cfg.registry = &peer_reg;
        assert(ecat_foe_core_init(&peer_core, &peer_cfg) == 0);
        assert(ecat_transfer_init(&peer_xfer, &peer_reg, &peer_core) == 0);

        uint8_t mbx_a_ctrl[2] = {0, 0}, mbx_b_ctrl[2] = {0, 0};
        uint8_t mbx_a_bufs[2 * 128], mbx_b_bufs[2 * 128];
        memset(mbx_a_bufs, 0, sizeof(mbx_a_bufs));
        memset(mbx_b_bufs, 0, sizeof(mbx_b_bufs));
        assert(ecat_foe_soes_set_mbx_context(&soes_adapter, mbx_a_ctrl, mbx_a_bufs, 2, 128) == 0);

        foe_file_cfg_t files_a[1] = {{ .name = "DRIVE_SERVO_MCU.efw", .max_data = 2048, .write_function = ecat_foe_soes_write_cb }};
        foe_cfg_t fcfg_a = { .n_files = 1, .buffer_size = 128, .files = files_a };

        /* 1. 主适配器 soes_adapter 开启合法 WRQ 会话 */
        assert(ecat_foe_soes_on_wrq(&soes_adapter, soes_mcu_fn, strlen(soes_mcu_fn)) == 0);
        assert(ecat_transfer_is_active(&mgr) == true);

        /* 2. 中途初始化空闲实例 peer_soes，并模拟 peer_soes 收到一条最终被原生栈以 ERR 拒绝的暂存 WRQ */
        ecat_foe_soes_init(&peer_soes, &peer_core, 128U);
        assert(ecat_foe_soes_bind_transfer(&peer_soes, &peer_xfer) == 0);
        assert(ecat_foe_soes_set_mbx_context(&peer_soes, mbx_b_ctrl, mbx_b_bufs, 2, 128) == 0);
        foe_file_cfg_t files_b[1] = {{ .name = "DRIVE_SERVO_MCU.efw", .max_data = 2048, .write_function = ecat_foe_soes_write_cb }};
        foe_cfg_t fcfg_b = { .n_files = 1, .buffer_size = 128, .files = files_b };

        size_t wrq_nlen = strlen(soes_mcu_fn);
        mbx_b_bufs[0] = (uint8_t)(6U + wrq_nlen);
        mbx_b_bufs[1] = 0U;
        mbx_b_bufs[5] = 0x04U; /* FoE */
        mbx_b_bufs[6] = 0x02U; /* WRQ */
        memcpy(mbx_b_bufs + 12U, soes_mcu_fn, wrq_nlen);
        assert(ecat_foe_soes_pre_process_hook(&peer_soes, &fcfg_b, mbx_b_bufs, 128U) == 0);
        /* 模拟原生 SOES 在输出槽 1 回复 FOE_OP_ERR (如非 BOOT 态 0x8008) */
        mbx_b_ctrl[1] = MBXstate_outreq;
        mbx_b_bufs[128 + 0] = 6U;
        mbx_b_bufs[128 + 5] = 0x04U;
        mbx_b_bufs[128 + 6] = 0x05U; /* ERR */
        mbx_b_bufs[128 + 8] = 0x08U;
        mbx_b_bufs[128 + 9] = 0x80U;
        assert(ecat_foe_soes_post_process_hook(&peer_soes, &fcfg_b, mbx_b_bufs + 128U, 128U) == 0);
        assert(ecat_transfer_is_active(&peer_xfer) == false);

        /* 3. 主适配器 soes_adapter 随后正常接收 FOE_OP_DATA 并写完整个 180 字节固件 */
        mbx_a_bufs[0] = 122U;
        mbx_a_bufs[1] = 0U;
        mbx_a_bufs[5] = 0x04U;
        mbx_a_bufs[6] = 0x03U; /* DATA */
        mbx_a_bufs[8] = 1U;
        assert(ecat_foe_soes_pre_process_hook(&soes_adapter, &fcfg_a, mbx_a_bufs, 128U) == 0);
        assert(ecat_foe_soes_write_cb(&files_a[0], fw_buf, 128U) == 0U);
        assert(ecat_foe_soes_pre_process_hook(&soes_adapter, &fcfg_a, mbx_a_bufs, 128U) == 0);
        assert(ecat_foe_soes_write_cb(&files_a[0], fw_buf + 128U, fw_len - 128U) == 0U);
        assert(core.state == ECAT_FOE_STATE_SUCCESS);
        assert(ecat_transfer_is_active(&mgr) == false);
    }

    printf(">>> 端到端多通道多目标双向传输测试全部通过！<<<\n");
    return 0;
}
