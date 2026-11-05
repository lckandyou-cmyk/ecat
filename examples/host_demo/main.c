/**
 * @file main.c
 * @brief 宿主机参考示例 (Host Reference Example)
 *
 * 本示例演示如何仅使用现有核心公开 API 对接文件后端 Flash 存储驱动，
 * 实现端到端固件升级生命周期调度，覆盖 8 大典型场景。
 *
 * 限制与边界说明:
 * 1. 宿主环境演示: 本示例运行于 macOS/Linux 宿主机环境，仅作为接口用法参考；
 * 2. 存储掉电安全: 文件后端存储驱动不具备嵌入式片内 Flash 的硬件断电原子性保护；
 * 3. 硬件与 MCU: 未在真实从站芯片 (LAN9252/ET1100) 或 MCU 裸机上执行；
 * 4. 协议栈接入: 未实现真实生产环境 SOES/SSC 接入；
 * 5. 纯现有公开 API: 完全依赖 ecat_foe_core.h，未增加或变动任何核心 API。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "ecat_foe_core.h"
#include "ecat_foe_efw.h"
#include "ecat_foe_target.h"

/* -------------------------------------------------------------------------- */
/*                         文件后端 Flash 驱动实现                             */
/* -------------------------------------------------------------------------- */

#define HOST_FLASH_DATA_FILE "host_flash_data.bin"
#define HOST_FLASH_META_FILE "host_flash_meta.bin"
#define HOST_FLASH_SECTOR_SIZE 4096U

typedef struct {
    uint16_t target_id;
    uint32_t erased_watermark;
    bool     is_initialized;
    bool     is_invalidated;
    bool     is_finalized;
    /* 故障注入 */
    bool     inject_fail_finalize;
} host_flash_driver_t;

static host_flash_driver_t s_host_drv;

static void clean_temp_files(void)
{
    remove(HOST_FLASH_DATA_FILE);
    remove(HOST_FLASH_META_FILE);
}

static int host_flash_init(void *user_ctx)
{
    host_flash_driver_t *drv = (host_flash_driver_t *)user_ctx;
    drv->is_initialized = true;
    drv->is_invalidated = false;
    drv->is_finalized = false;
    drv->erased_watermark = 0;

    FILE *fp = fopen(HOST_FLASH_DATA_FILE, "wb");
    if (!fp) return -1;
    fclose(fp);

    printf("[DRV] init(target=%u) -> 0\n", drv->target_id);
    return 0;
}

static int host_flash_invalidate(void *user_ctx)
{
    host_flash_driver_t *drv = (host_flash_driver_t *)user_ctx;
    drv->is_invalidated = true;

    /* 在元数据文件中写入 0 表示固件处于无效状态 */
    FILE *fp = fopen(HOST_FLASH_META_FILE, "wb");
    if (!fp) return -1;
    uint32_t flag = 0x00000000U;
    fwrite(&flag, sizeof(flag), 1, fp);
    fclose(fp);

    printf("[DRV] invalidate(target=%u) -> 0\n", drv->target_id);
    return 0;
}

static int host_flash_erase_to(uint32_t need_end, uint32_t *erased_end, void *user_ctx)
{
    host_flash_driver_t *drv = (host_flash_driver_t *)user_ctx;

    /* 按 4096 字节扇区向上对齐擦除边界 */
    uint32_t new_watermark = ((need_end + HOST_FLASH_SECTOR_SIZE - 1U) / HOST_FLASH_SECTOR_SIZE) * HOST_FLASH_SECTOR_SIZE;
    if (new_watermark < need_end) {
        new_watermark = need_end;
    }

    FILE *fp = fopen(HOST_FLASH_DATA_FILE, "r+b");
    if (!fp) {
        fp = fopen(HOST_FLASH_DATA_FILE, "w+b");
        if (!fp) return -1;
    }

    fseek(fp, drv->erased_watermark, SEEK_SET);
    uint32_t bytes_to_fill = new_watermark - drv->erased_watermark;
    for (uint32_t i = 0; i < bytes_to_fill; i++) {
        fputc(0xFF, fp);
    }
    fflush(fp);
    fclose(fp);

    drv->erased_watermark = new_watermark;
    if (erased_end) {
        *erased_end = new_watermark;
    }

    printf("[DRV] erase_to(need=%u) -> erased=%u\n", need_end, new_watermark);
    return 0;
}

static int host_flash_write(uint32_t offset, const uint8_t *buf, uint32_t len, void *user_ctx)
{
    (void)user_ctx;
    FILE *fp = fopen(HOST_FLASH_DATA_FILE, "r+b");
    if (!fp) return -1;

    fseek(fp, offset, SEEK_SET);
    size_t written = fwrite(buf, 1, len, fp);
    fflush(fp);
    fclose(fp);

    if (written != len) return -1;
    printf("[DRV] write(offset=%u, len=%u) -> 0\n", offset, len);
    return 0;
}

static int host_flash_read(uint32_t offset, uint8_t *buf, uint32_t len, void *user_ctx)
{
    (void)user_ctx;
    FILE *fp = fopen(HOST_FLASH_DATA_FILE, "rb");
    if (!fp) return -1;

    fseek(fp, offset, SEEK_SET);
    size_t read_bytes = fread(buf, 1, len, fp);
    fclose(fp);

    if (read_bytes != len) return -1;
    printf("[DRV] read(offset=%u, len=%u) -> 0\n", offset, len);
    return 0;
}

static int host_flash_finalize(void *user_ctx)
{
    host_flash_driver_t *drv = (host_flash_driver_t *)user_ctx;
    if (drv->inject_fail_finalize) {
        printf("[DRV] finalize(target=%u) -> FAIL (-1)\n", drv->target_id);
        return -1;
    }

    /* 成功提交: 写入有效标记 0x5A5A5A5A */
    FILE *fp = fopen(HOST_FLASH_META_FILE, "wb");
    if (!fp) return -1;
    uint32_t flag = 0x5A5A5A5AU;
    fwrite(&flag, sizeof(flag), 1, fp);
    fclose(fp);

    drv->is_finalized = true;
    printf("[DRV] finalize(target=%u) -> 0\n", drv->target_id);
    return 0;
}

static void host_flash_abort(void *user_ctx)
{
    host_flash_driver_t *drv = (host_flash_driver_t *)user_ctx;
    drv->is_initialized = false;
    drv->is_finalized = false;

    /* 确保有效标记被抹除 */
    FILE *fp = fopen(HOST_FLASH_META_FILE, "wb");
    if (fp) {
        uint32_t flag = 0x00000000U;
        fwrite(&flag, sizeof(flag), 1, fp);
        fclose(fp);
    }
    printf("[DRV] abort\n");
}

static const ecat_foe_target_driver_t s_target_ops = {
    .init       = host_flash_init,
    .invalidate = host_flash_invalidate,
    .erase_to   = host_flash_erase_to,
    .write      = host_flash_write,
    .read       = host_flash_read,
    .finalize   = host_flash_finalize,
    .abort      = host_flash_abort,
};

/* 核心防回滚端口桩实现 */
int port_get_version_floor(uint16_t target_id, uint32_t *floor_ver)
{
    (void)target_id;
    if (floor_ver) *floor_ver = 0;
    return 0;
}

int port_set_version_floor(uint16_t target_id, uint32_t new_floor)
{
    (void)target_id;
    (void)new_floor;
    return 0;
}

/* -------------------------------------------------------------------------- */
/*                         固件流构造辅助函数                                 */
/* -------------------------------------------------------------------------- */

static size_t build_efw_buffer(
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
    out_buf[0] = 0x45; out_buf[1] = 0x43; out_buf[2] = 0x46; out_buf[3] = 0x57; /* "ECFW" */
    out_buf[4] = 0x01; out_buf[5] = 0x00; /* format_version = 1 */
    out_buf[6] = 0x40; out_buf[7] = 0x00; /* header_length = 64 */

    out_buf[8] = (uint8_t)(project_id & 0xFF);
    out_buf[9] = (uint8_t)((project_id >> 8) & 0xFF);
    out_buf[10] = (uint8_t)((project_id >> 16) & 0xFF);
    out_buf[11] = (uint8_t)((project_id >> 24) & 0xFF);

    out_buf[12] = (uint8_t)(hardware_id & 0xFF);
    out_buf[13] = (uint8_t)((hardware_id >> 8) & 0xFF);
    out_buf[14] = (uint8_t)((hardware_id >> 16) & 0xFF);
    out_buf[15] = (uint8_t)((hardware_id >> 24) & 0xFF);

    out_buf[16] = (uint8_t)(hw_ver_min & 0xFF);
    out_buf[17] = (uint8_t)((hw_ver_min >> 8) & 0xFF);
    out_buf[18] = (uint8_t)(hw_ver_max & 0xFF);
    out_buf[19] = (uint8_t)((hw_ver_max >> 8) & 0xFF);

    out_buf[20] = (uint8_t)(target_id & 0xFF);
    out_buf[21] = (uint8_t)((target_id >> 8) & 0xFF);
    out_buf[22] = (uint8_t)(flags & 0xFF);
    out_buf[23] = (uint8_t)((flags >> 8) & 0xFF);

    out_buf[24] = (uint8_t)(fw_version & 0xFF);
    out_buf[25] = (uint8_t)((fw_version >> 8) & 0xFF);
    out_buf[26] = (uint8_t)((fw_version >> 16) & 0xFF);
    out_buf[27] = (uint8_t)((fw_version >> 24) & 0xFF);

    out_buf[28] = (uint8_t)(min_allow_version & 0xFF);
    out_buf[29] = (uint8_t)((min_allow_version >> 8) & 0xFF);
    out_buf[30] = (uint8_t)((min_allow_version >> 16) & 0xFF);
    out_buf[31] = (uint8_t)((min_allow_version >> 24) & 0xFF);

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

static void setup_core_config(ecat_foe_core_cfg_t *cfg, host_flash_driver_t *drv)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->project_id = 0x10010001U;
    cfg->project_name = "SERVO_DRV";
    cfg->hardware_id = 0x00000001U;
    cfg->local_hw_ver = 1U;
    cfg->anti_rollback_enable = false;
    cfg->max_header_size = 128U;

    cfg->targets[0].target_id = 1;
    cfg->targets[0].enabled = true;
    cfg->targets[0].max_payload_size = 65536U;
    cfg->targets[0].write_unit = 8U;
    cfg->targets[0].driver = &s_target_ops;
    cfg->targets[0].user_ctx = drv;
}

static const char *err_to_str(ecat_foe_core_err_t err)
{
    switch (err) {
        case ECAT_FOE_CORE_OK: return "NONE";
        case ECAT_FOE_CORE_ERR_WRQ_PROJECT_MISMATCH: return "WRQ_PROJECT_MISMATCH";
        case ECAT_FOE_CORE_ERR_HDR_CRC: return "HDR_CRC";
        case ECAT_FOE_CORE_ERR_OVER_RECEIVE: return "OVER_RECEIVE";
        case ECAT_FOE_CORE_ERR_UNDER_RECEIVE: return "UNDER_RECEIVE";
        case ECAT_FOE_CORE_ERR_DRV_FINALIZE: return "DRV_FINALIZE";
        default: return "OTHER_ERROR";
    }
}

static const char *state_to_str(ecat_foe_state_t st)
{
    switch (st) {
        case ECAT_FOE_STATE_IDLE: return "IDLE";
        case ECAT_FOE_STATE_WRQ_ACKED: return "WRQ_ACKED";
        case ECAT_FOE_STATE_DATA: return "DATA";
        case ECAT_FOE_STATE_SUCCESS: return "SUCCESS";
        case ECAT_FOE_STATE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

/* -------------------------------------------------------------------------- */
/*                                8 大场景实现                                */
/* -------------------------------------------------------------------------- */

static uint8_t s_efw_buf[512];

/* 场景 1: 正常成功升级 */
static void run_scenario_success(void)
{
    printf("=== Scenario 1: Normal Success ===\n");
    clean_temp_files();
    memset(&s_host_drv, 0, sizeof(s_host_drv));
    s_host_drv.target_id = 1;

    ecat_foe_core_t core;
    ecat_foe_core_cfg_t cfg;
    setup_core_config(&cfg, &s_host_drv);
    assert(ecat_foe_core_init(&core, &cfg) == 0);

    const char *fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
    int wrq_st = ecat_foe_core_on_wrq(&core, fn, strlen(fn));
    printf("[CORE] on_wrq: %s -> %s\n", fn, (wrq_st == 0) ? "OK" : "FAIL");

    static const uint8_t payload[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                        0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    size_t efw_len = build_efw_buffer(s_efw_buf, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    /* 按 32 字节分块推送数据 */
    size_t offset = 0;
    while (offset < efw_len) {
        size_t chunk = (efw_len - offset >= 32) ? 32 : (efw_len - offset);
        int d_st = ecat_foe_core_on_data(&core, s_efw_buf + offset, chunk);
        assert(d_st == 0);
        offset += chunk;
    }

    int end_st = ecat_foe_core_on_end(&core);
    printf("[CORE] on_end -> %s\n", (end_st == 0) ? "OK" : "FAIL");
    printf("[CORE] Final State: %s, Last Error: %s\n\n", state_to_str(core.state), err_to_str(core.last_error));
}

/* 场景 2: 头部 CRC 错误 */
static void run_scenario_header_crc_mismatch(void)
{
    printf("=== Scenario 2: Header CRC Mismatch ===\n");
    clean_temp_files();
    memset(&s_host_drv, 0, sizeof(s_host_drv));
    s_host_drv.target_id = 1;

    ecat_foe_core_t core;
    ecat_foe_core_cfg_t cfg;
    setup_core_config(&cfg, &s_host_drv);
    assert(ecat_foe_core_init(&core, &cfg) == 0);

    const char *fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
    ecat_foe_core_on_wrq(&core, fn, strlen(fn));

    static const uint8_t payload[16] = {0xAA, 0xBB};
    size_t efw_len = build_efw_buffer(s_efw_buf, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    /* 翻转 header CRC 字节 */
    s_efw_buf[60] ^= 0xFF;

    int d_st = ecat_foe_core_on_data(&core, s_efw_buf, efw_len);
    printf("[CORE] on_data (corrupt header CRC) -> %s\n", (d_st == 0) ? "OK" : "REJECT");
    printf("[CORE] Final State: %s, Last Error: %s\n\n", state_to_str(core.state), err_to_str(core.last_error));
}

/* 场景 3: 项目 ID 不符 */
static void run_scenario_project_id_mismatch(void)
{
    printf("=== Scenario 3: Project ID Mismatch ===\n");
    clean_temp_files();
    memset(&s_host_drv, 0, sizeof(s_host_drv));
    s_host_drv.target_id = 1;

    ecat_foe_core_t core;
    ecat_foe_core_cfg_t cfg;
    setup_core_config(&cfg, &s_host_drv);
    assert(ecat_foe_core_init(&core, &cfg) == 0);

    const char *fn = "OTHER_PRJ_MCU_V1.0.0.0.efw";
    int wrq_st = ecat_foe_core_on_wrq(&core, fn, strlen(fn));
    printf("[CORE] on_wrq: %s -> %s\n", fn, (wrq_st == 0) ? "OK" : "REJECT");
    printf("[CORE] Final State: %s, Last Error: %s\n\n", state_to_str(core.state), err_to_str(core.last_error));
}

/* 场景 4: 数据流被截断 (Under Receive) */
static void run_scenario_stream_under_receive(void)
{
    printf("=== Scenario 4: Stream Under-Receive ===\n");
    clean_temp_files();
    memset(&s_host_drv, 0, sizeof(s_host_drv));
    s_host_drv.target_id = 1;

    ecat_foe_core_t core;
    ecat_foe_core_cfg_t cfg;
    setup_core_config(&cfg, &s_host_drv);
    assert(ecat_foe_core_init(&core, &cfg) == 0);

    const char *fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
    ecat_foe_core_on_wrq(&core, fn, strlen(fn));

    static const uint8_t payload[32] = {1, 2, 3};
    size_t efw_len = build_efw_buffer(s_efw_buf, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 32);
    (void)efw_len;

    /* 仅喂入 64 字节头 + 8 字节数据 (缺少 24 字节) */
    ecat_foe_core_on_data(&core, s_efw_buf, 72);
    int end_st = ecat_foe_core_on_end(&core);
    printf("[CORE] on_end (truncated stream) -> %s\n", (end_st == 0) ? "OK" : "REJECT");
    printf("[CORE] Final State: %s, Last Error: %s\n\n", state_to_str(core.state), err_to_str(core.last_error));
}

/* 场景 5: 多收数据即时熔断 (Over Receive) */
static void run_scenario_stream_over_receive(void)
{
    printf("=== Scenario 5: Stream Over-Receive ===\n");
    clean_temp_files();
    memset(&s_host_drv, 0, sizeof(s_host_drv));
    s_host_drv.target_id = 1;

    ecat_foe_core_t core;
    ecat_foe_core_cfg_t cfg;
    setup_core_config(&cfg, &s_host_drv);
    assert(ecat_foe_core_init(&core, &cfg) == 0);

    const char *fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
    ecat_foe_core_on_wrq(&core, fn, strlen(fn));

    static const uint8_t payload[16] = {1, 2};
    size_t efw_len = build_efw_buffer(s_efw_buf, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    /* 先喂入完整的 80 字节 */
    ecat_foe_core_on_data(&core, s_efw_buf, efw_len);
    /* 故意再塞入多余的 8 字节数据 */
    uint8_t extra[8] = {0xFF};
    int d_st = ecat_foe_core_on_data(&core, extra, sizeof(extra));
    printf("[CORE] on_data (overflow data) -> %s\n", (d_st == 0) ? "OK" : "REJECT");
    printf("[CORE] Final State: %s, Last Error: %s\n\n", state_to_str(core.state), err_to_str(core.last_error));
}

/* 场景 6: 中途断流后在同一个 core 实例上重试成功 */
static void run_scenario_abort_retry_same_core(void)
{
    printf("=== Scenario 6: Abort and Retry on Same Core ===\n");
    clean_temp_files();
    memset(&s_host_drv, 0, sizeof(s_host_drv));
    s_host_drv.target_id = 1;

    ecat_foe_core_t core;
    ecat_foe_core_cfg_t cfg;
    setup_core_config(&cfg, &s_host_drv);
    assert(ecat_foe_core_init(&core, &cfg) == 0);

    const char *fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
    /* 第 1 次尝试: 握手并写入首包后断流中止 */
    ecat_foe_core_on_wrq(&core, fn, strlen(fn));
    static const uint8_t payload[16] = {0x11, 0x22};
    size_t efw_len = build_efw_buffer(s_efw_buf, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    ecat_foe_core_on_data(&core, s_efw_buf, 64);
    printf("[CORE] Simulated stream break -> calling on_abort\n");
    ecat_foe_core_on_abort(&core);
    printf("[CORE] Intermediate State: %s\n", state_to_str(core.state));

    /* 第 2 次尝试: 同一个 core 实例接收新 WRQ 重新开始 */
    printf("[CORE] Retrying with new WRQ on same core instance...\n");
    int wrq_retry = ecat_foe_core_on_wrq(&core, fn, strlen(fn));
    printf("[CORE] on_wrq retry -> %s\n", (wrq_retry == 0) ? "OK" : "FAIL");

    ecat_foe_core_on_data(&core, s_efw_buf, efw_len);
    int end_retry = ecat_foe_core_on_end(&core);
    printf("[CORE] on_end retry -> %s\n", (end_retry == 0) ? "OK" : "FAIL");
    printf("[CORE] Final State: %s, Last Error: %s\n\n", state_to_str(core.state), err_to_str(core.last_error));
}

/* 场景 7: 补零避开空包路径 */
static void run_scenario_padded_efw(void)
{
    printf("=== Scenario 7: Padded Firmware (Avoid Zero-Len) ===\n");
    clean_temp_files();
    memset(&s_host_drv, 0, sizeof(s_host_drv));
    s_host_drv.target_id = 1;

    ecat_foe_core_t core;
    ecat_foe_core_cfg_t cfg;
    setup_core_config(&cfg, &s_host_drv);
    assert(ecat_foe_core_init(&core, &cfg) == 0);

    const char *fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
    ecat_foe_core_on_wrq(&core, fn, strlen(fn));

    /* 设定总长 80 字节 (64 头 + 16 payload)，在 64 字节包载荷下末包为 16 字节短包 */
    static const uint8_t payload[16] = {0x55, 0x66};
    size_t efw_len = build_efw_buffer(s_efw_buf, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    ecat_foe_core_on_data(&core, s_efw_buf, efw_len);
    int end_st = ecat_foe_core_on_end(&core);
    printf("[CORE] on_end (short packet path) -> %s\n", (end_st == 0) ? "OK" : "FAIL");
    printf("[CORE] Final State: %s, Last Error: %s\n\n", state_to_str(core.state), err_to_str(core.last_error));
}

/* 场景 8: 零长度路径失败不可见演示 (标限制) */
static void run_scenario_zero_length_limitation_demo(void)
{
    printf("=== Scenario 8: Zero-Length Failure Invisibility Demo (Limitation) ===\n");
    clean_temp_files();
    memset(&s_host_drv, 0, sizeof(s_host_drv));
    s_host_drv.target_id = 1;
    /* 注入底层驱动 finalize 失败 */
    s_host_drv.inject_fail_finalize = true;

    ecat_foe_core_t core;
    ecat_foe_core_cfg_t cfg;
    setup_core_config(&cfg, &s_host_drv);
    assert(ecat_foe_core_init(&core, &cfg) == 0);

    const char *fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
    ecat_foe_core_on_wrq(&core, fn, strlen(fn));

    static const uint8_t payload[16] = {0x77, 0x88};
    size_t efw_len = build_efw_buffer(s_efw_buf, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    ecat_foe_core_on_data(&core, s_efw_buf, efw_len);

    /* 核心机在 on_end 检测到 finalize 失败，返回 -1 并置 ERROR */
    int end_st = ecat_foe_core_on_end(&core);
    printf("[CORE] on_end (finalize injected failure) -> %s\n", (end_st == 0) ? "OK" : "REJECT");
    printf("[CORE] State: %s, Last Error: %s\n", state_to_str(core.state), err_to_str(core.last_error));
    printf("[NOTE] 在原生 SOES 的 0 长度包结束分支中，FOE_fclose 返回值被忽略并回 ACK；\n");
    printf("       核心机虽处于 ERROR 态且有效标记未置位，但主站协议栈无法收到 0x800B 中止 (限制演示)。\n\n");
}

int main(void)
{
    printf("============================================================\n");
    printf(" EtherCAT FoE Core Host Reference Demo (8 Scenarios)\n");
    printf("============================================================\n\n");

    run_scenario_success();
    run_scenario_header_crc_mismatch();
    run_scenario_project_id_mismatch();
    run_scenario_stream_under_receive();
    run_scenario_stream_over_receive();
    run_scenario_abort_retry_same_core();
    run_scenario_padded_efw();
    run_scenario_zero_length_limitation_demo();

    clean_temp_files();
    printf("============================================================\n");
    printf(" All 8 Host Scenarios Finished Successfully.\n");
    printf("============================================================\n");
    return 0;
}
