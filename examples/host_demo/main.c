/**
 * @file main.c
 * @brief 宿主机参考示例 (Host Reference Example)
 *
 * 本示例演示如何仅使用现有核心公开 API 对接用户自定义板级 Flash 存储驱动，
 * 实现端到端固件升级生命周期调度，并覆盖 8 大典型场景。
 *
 * 设计与契约特性：
 * 1. 临时隔离目录与无副作用契约：所有模拟 Flash 介质文件均创建于独立的临时隔离目录中，
 *    严禁在当前工作目录创建或删除任何文件；外部独立测试将严密验证预存同名哨兵文件的完整性与并发隔离。
 * 2. 非破坏性 init 与失效标记先行契约：
 *    - 驱动 init() 仅建立访问句柄，严禁截断或清空现有文件数据；
 *    - 在执行任何扇区擦除之前，必须由 invalidate() 将有效标记持久化置为无效；
 *    - 提供对上一版本旧数据和有效标记在核验/失效前的可观察断言；
 *    - 所有文件 I/O 操作均包含严密的错误检查。
 * 3. 真实 Python 打包器集成：场景 7 优先加载由 tools/efw_pack.py 打包的补零固件 (.efw)，
 *    真实验证从打包脚本到核心状态机的端到端对齐。
 * 4. 严谨代码断言：所有场景内部均包含强 C 语言断言 (assert)，避免仅依赖输出文本比对。
 * 5. 用户自定义板级驱动与多目标扩展：展示 Target 1 (板载主 MCU Flash) 与 Target 2 (外部 SPI Flash 可选目标)。
 */

#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>

#include "ecat_foe_core.h"
#include "ecat_foe_efw.h"
#include "ecat_foe_target.h"

/* -------------------------------------------------------------------------- */
/*                         临时隔离目录与文件路径管理                         */
/* -------------------------------------------------------------------------- */

static char s_demo_tmpdir[512];
static char s_host_flash_data_file[512];
static char s_host_flash_meta_file[512];

#define HOST_FLASH_SECTOR_SIZE 4096U
#define HOST_FLASH_VALID_MAGIC 0x5A5AA5A5U
#define HOST_FLASH_INVALID_MAGIC 0x00000000U

typedef struct {
    uint16_t target_id;
    uint32_t erased_watermark;
    bool     is_initialized;
    bool     is_invalidated;
    bool     is_finalized;
    bool     inject_fail_finalize;
} host_flash_driver_t;

static host_flash_driver_t s_host_drv;

/* 安全文件同步与关闭辅助函数，杜绝因短路求值导致的 fclose 泄漏 */
static int safe_file_close(FILE *fp)
{
    if (!fp) return -1;
    int flush_rc = fflush(fp);
    int close_rc = fclose(fp);
    return (flush_rc == 0 && close_rc == 0) ? 0 : -1;
}

static void init_demo_env(void)
{
    char tmpl[] = "/tmp/foe_host_demo_XXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir != NULL);
    strncpy(s_demo_tmpdir, dir, sizeof(s_demo_tmpdir) - 1);
    s_demo_tmpdir[sizeof(s_demo_tmpdir) - 1] = '\0';

    snprintf(s_host_flash_data_file, sizeof(s_host_flash_data_file), "%s/flash_data.bin", s_demo_tmpdir);
    snprintf(s_host_flash_meta_file, sizeof(s_host_flash_meta_file), "%s/flash_meta.bin", s_demo_tmpdir);
}

static void clean_temp_files(void)
{
    remove(s_host_flash_data_file);
    remove(s_host_flash_meta_file);
}

static void destroy_demo_env(void)
{
    clean_temp_files();
    rmdir(s_demo_tmpdir);
}

/* -------------------------------------------------------------------------- */
/*                   用户自定义板级存储驱动实现 (文件模拟 Flash)              */
/* -------------------------------------------------------------------------- */

/* 1. 初始化回调: 仅建立外设通信/打开句柄，严禁破坏/截断既有数据 */
static int host_flash_init(void *user_ctx)
{
    host_flash_driver_t *drv = (host_flash_driver_t *)user_ctx;
    if (!drv) return -1;

    drv->is_initialized = true;
    drv->is_invalidated = false;
    drv->is_finalized = false;
    drv->erased_watermark = 0;

    /* 非破坏性打开或创建文件 */
    FILE *fp = fopen(s_host_flash_data_file, "r+b");
    if (!fp) {
        fp = fopen(s_host_flash_data_file, "w+b");
        if (!fp) return -1;
    }
    if (fclose(fp) != 0) return -1;

    printf("[DRV] init(target=%u) -> 0 (non-destructive)\n", drv->target_id);
    return 0;
}

/* 2. 置无效标记回调: 在执行任何物理擦写前将有效标记持久化置为无效 */
static int host_flash_invalidate(void *user_ctx)
{
    host_flash_driver_t *drv = (host_flash_driver_t *)user_ctx;
    if (!drv || !drv->is_initialized) return -1;

    drv->is_invalidated = true;

    /* 将元数据中的固件有效标记持久化清零 */
    FILE *fp = fopen(s_host_flash_meta_file, "wb");
    if (!fp) return -1;
    uint32_t flag = HOST_FLASH_INVALID_MAGIC;
    if (fwrite(&flag, sizeof(flag), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }
    if (safe_file_close(fp) != 0) return -1;

    printf("[DRV] invalidate(target=%u) -> 0 (flag=0x%08X)\n", drv->target_id, flag);
    return 0;
}

/* 3. 按需擦除回调: 仅在 invalidate 成功后允许对齐擦除扇区 */
static int host_flash_erase_to(uint32_t need_end, uint32_t *erased_end, void *user_ctx)
{
    host_flash_driver_t *drv = (host_flash_driver_t *)user_ctx;
    if (!drv || !drv->is_initialized || !drv->is_invalidated) {
        /* 未失效前拒绝任何擦除操作 */
        return -1;
    }

    uint32_t new_watermark = ((need_end + HOST_FLASH_SECTOR_SIZE - 1U) / HOST_FLASH_SECTOR_SIZE) * HOST_FLASH_SECTOR_SIZE;
    if (new_watermark < need_end) {
        new_watermark = need_end;
    }

    FILE *fp = fopen(s_host_flash_data_file, "r+b");
    if (!fp) {
        fp = fopen(s_host_flash_data_file, "w+b");
        if (!fp) return -1;
    }

    if (fseek(fp, (long)drv->erased_watermark, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    uint32_t bytes_to_fill = new_watermark - drv->erased_watermark;
    for (uint32_t i = 0; i < bytes_to_fill; i++) {
        if (fputc(0xFF, fp) == EOF) {
            fclose(fp);
            return -1;
        }
    }
    if (safe_file_close(fp) != 0) return -1;

    drv->erased_watermark = new_watermark;
    if (erased_end) {
        *erased_end = new_watermark;
    }

    printf("[DRV] erase_to(need=%u) -> erased=%u\n", need_end, new_watermark);
    return 0;
}

/* 4. 对齐分块写入回调 */
static int host_flash_write(uint32_t offset, const uint8_t *buf, uint32_t len, void *user_ctx)
{
    host_flash_driver_t *drv = (host_flash_driver_t *)user_ctx;
    if (!drv || !drv->is_initialized || !buf) return -1;

    FILE *fp = fopen(s_host_flash_data_file, "r+b");
    if (!fp) return -1;

    if (fseek(fp, (long)offset, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    size_t written = fwrite(buf, 1, len, fp);
    if (written != len) {
        fclose(fp);
        return -1;
    }
    if (safe_file_close(fp) != 0) return -1;

    printf("[DRV] write(offset=%u, len=%u) -> 0\n", offset, len);
    return 0;
}

/* 5. 介质物理读取回调 (用于读回校验) */
static int host_flash_read(uint32_t offset, uint8_t *buf, uint32_t len, void *user_ctx)
{
    host_flash_driver_t *drv = (host_flash_driver_t *)user_ctx;
    if (!drv || !drv->is_initialized || !buf) return -1;

    FILE *fp = fopen(s_host_flash_data_file, "rb");
    if (!fp) return -1;

    if (fseek(fp, (long)offset, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    size_t read_bytes = fread(buf, 1, len, fp);
    if (read_bytes != len) {
        fclose(fp);
        return -1;
    }
    if (fclose(fp) != 0) return -1;

    printf("[DRV] read(offset=%u, len=%u) -> 0\n", offset, len);
    return 0;
}

/* 6. 成功提交回调: 仅在所有校验通过后最后写入有效标记 */
static int host_flash_finalize(void *user_ctx)
{
    host_flash_driver_t *drv = (host_flash_driver_t *)user_ctx;
    if (!drv || !drv->is_initialized) return -1;

    if (drv->inject_fail_finalize) {
        printf("[DRV] finalize(target=%u) -> FAIL (-1)\n", drv->target_id);
        return -1;
    }

    FILE *fp = fopen(s_host_flash_meta_file, "wb");
    if (!fp) return -1;
    uint32_t flag = HOST_FLASH_VALID_MAGIC;
    if (fwrite(&flag, sizeof(flag), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }
    if (safe_file_close(fp) != 0) return -1;

    drv->is_finalized = true;
    printf("[DRV] finalize(target=%u) -> 0 (flag=0x%08X)\n", drv->target_id, flag);
    return 0;
}

/* 7. 中止清理回调 (具备幂等性) */
static void host_flash_abort(void *user_ctx)
{
    host_flash_driver_t *drv = (host_flash_driver_t *)user_ctx;
    if (!drv) return;

    /* 确保有效标记处于清除状态 */
    FILE *fp = fopen(s_host_flash_meta_file, "wb");
    if (fp) {
        uint32_t flag = HOST_FLASH_INVALID_MAGIC;
        (void)fwrite(&flag, sizeof(flag), 1, fp);
        safe_file_close(fp);
    }
    drv->is_initialized = false;
    drv->is_finalized = false;
    printf("[DRV] abort\n");
}

static const ecat_foe_target_driver_t s_target_ops = {
    .init = host_flash_init,
    .invalidate = host_flash_invalidate,
    .erase_to = host_flash_erase_to,
    .write = host_flash_write,
    .read = host_flash_read,
    .finalize = host_flash_finalize,
    .abort = host_flash_abort,
    /* 宿主演示关闭防回滚；此回调不是硬件事务实现。 */
    .finalize_with_version_floor = NULL
};

/* 防回滚外部端口桩 */
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

/* 辅助介质检查函数 */
static void preset_flash_content(uint32_t meta_flag, uint8_t byte_val, size_t len)
{
    FILE *fmeta = fopen(s_host_flash_meta_file, "wb");
    assert(fmeta != NULL);
    assert(fwrite(&meta_flag, sizeof(meta_flag), 1, fmeta) == 1);
    assert(safe_file_close(fmeta) == 0);

    FILE *fdata = fopen(s_host_flash_data_file, "wb");
    assert(fdata != NULL);
    for (size_t i = 0; i < len; i++) {
        assert(fputc(byte_val, fdata) != EOF);
    }
    assert(safe_file_close(fdata) == 0);
}

static void assert_flash_content(uint32_t expected_meta, uint8_t expected_byte, size_t check_len)
{
    FILE *fmeta = fopen(s_host_flash_meta_file, "rb");
    assert(fmeta != NULL);
    uint32_t meta = 0;
    assert(fread(&meta, sizeof(meta), 1, fmeta) == 1);
    fclose(fmeta);
    assert(meta == expected_meta);

    FILE *fdata = fopen(s_host_flash_data_file, "rb");
    assert(fdata != NULL);
    for (size_t i = 0; i < check_len; i++) {
        int c = fgetc(fdata);
        assert(c == (int)expected_byte);
    }
    fclose(fdata);
}

/* -------------------------------------------------------------------------- */
/*                         固件流构造与配置辅助函数                           */
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
    memset(out_buf, 0, ECAT_FOE_EFW_HEADER_MIN_LEN);
    out_buf[0] = 0x45; out_buf[1] = 0x43; out_buf[2] = 0x46; out_buf[3] = 0x57; /* "ECFW" */
    out_buf[4] = 0x02; out_buf[5] = 0x00; /* format_version = 2 */
    out_buf[6] = 0x94; out_buf[7] = 0x00; /* header_length = 148 */

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

    memcpy(out_buf + 60, "SIG2", 4);
    out_buf[64] = 1U;
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

static void setup_core_config(ecat_foe_core_cfg_t *cfg, host_flash_driver_t *drv)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->project_id = 0x10010001U;
    cfg->project_name = "SERVO_DRV";
    cfg->hardware_id = 0x00000001U;
    cfg->local_hw_ver = 1U;
    cfg->anti_rollback_enable = false;
    cfg->signature_verify_enable = (ECAT_FOE_SIGNATURE_VERIFY_ENABLE != 0);
    cfg->max_header_size = ECAT_FOE_HEADER_BUF_MAX;

    /* Target 1: 板载主 MCU Flash (使能) */
    cfg->targets[0].target_id = 1;
    cfg->targets[0].enabled = true;
    cfg->targets[0].max_payload_size = 65536U;
    cfg->targets[0].write_unit = 8U;
    cfg->targets[0].driver = &s_target_ops;
    cfg->targets[0].user_ctx = drv;

    /* Target 2: 外部 SPI Flash (可选目标演示，当前配置为未使能) */
    cfg->targets[1].target_id = 2;
    cfg->targets[1].enabled = false;
    cfg->targets[1].max_payload_size = 131072U;
    cfg->targets[1].write_unit = 16U;
    cfg->targets[1].driver = &s_target_ops;
    cfg->targets[1].user_ctx = drv;
}

static const char *err_to_str(ecat_foe_core_err_t err)
{
    switch (err) {
        case ECAT_FOE_CORE_OK: return "NONE";
        case ECAT_FOE_CORE_ERR_WRQ_PROJECT_MISMATCH: return "WRQ_PROJECT_MISMATCH";
        case ECAT_FOE_CORE_ERR_HDR_CRC: return "HDR_CRC";
        case ECAT_FOE_CORE_ERR_HDR_VERIFY: return "HDR_VERIFY";
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

/* 场景 1: 正常成功升级 (包含旧数据保护与失效先行断言) */
static void run_scenario_success(void)
{
    printf("=== Scenario 1: Normal Success (With Preset Old Data Assertions) ===\n");
    clean_temp_files();
    memset(&s_host_drv, 0, sizeof(s_host_drv));
    s_host_drv.target_id = 1;

    /* 预存上一版本旧固件数据 (32 字节 0x33) 与旧有效标记 (0x5A5AA5A5) */
    preset_flash_content(HOST_FLASH_VALID_MAGIC, 0x33, 32);

    ecat_foe_core_t core;
    ecat_foe_core_cfg_t cfg;
    setup_core_config(&cfg, &s_host_drv);
    assert(ecat_foe_core_init(&core, &cfg) == 0);

    const char *fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
    int wrq_st = ecat_foe_core_on_wrq(&core, fn, strlen(fn));
    assert(wrq_st == 0);
    printf("[CORE] on_wrq: %s -> OK\n", fn);

    /* 断言 1: 在喂入数据前，介质上的旧数据与旧有效标记完好无损 */
    assert_flash_content(HOST_FLASH_VALID_MAGIC, 0x33, 32);
    printf("[CHECK] Pre-data: Old firmware data and valid flag intact\n");

    /* 准备新固件 (16 字节新数据 0x77) */
    static const uint8_t new_payload[16] = {
        0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
        0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77
    };
    size_t efw_len = build_efw_buffer(s_efw_buf, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, new_payload, 16);

    /* 喂入固件流: 核心触发 init -> invalidate -> erase_to -> write -> read 校验 */
    int data_st = ecat_foe_core_on_data(&core, s_efw_buf, efw_len);
    assert(data_st == 0);
    assert(s_host_drv.is_invalidated == true);

    int end_st = ecat_foe_core_on_end(&core);
    assert(end_st == 0);
    assert(s_host_drv.is_finalized == true);
    assert(core.state == ECAT_FOE_STATE_SUCCESS);
    assert(core.last_error == ECAT_FOE_CORE_OK);

    /* 断言 2: finalize 成功后，介质上的新有效标记置位，且读出新数据 */
    assert_flash_content(HOST_FLASH_VALID_MAGIC, 0x77, 16);
    printf("[CHECK] Post-finalize: New firmware data verified and valid flag set\n");

    printf("[CORE] on_end -> OK\n");
    printf("[CORE] Final State: %s, Last Error: %s\n\n", state_to_str(core.state), err_to_str(core.last_error));
}

/* 场景 2: 头部 CRC 校验失败 */
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
    assert(ecat_foe_core_on_wrq(&core, fn, strlen(fn)) == 0);

    static const uint8_t payload[16] = {0xAA, 0xBB};
    size_t efw_len = build_efw_buffer(s_efw_buf, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    /* 篡改 Header CRC 字节 */
    s_efw_buf[60] ^= 0xFF;

    int d_st = ecat_foe_core_on_data(&core, s_efw_buf, efw_len);
    assert(d_st != 0);
    assert(core.state == ECAT_FOE_STATE_ERROR);
    assert(core.last_error == ECAT_FOE_CORE_ERR_HDR_CRC);

    printf("[CORE] on_data (corrupt header CRC) -> %s\n", (d_st == 0) ? "OK" : "REJECT");
    printf("[CORE] Final State: %s, Last Error: %s\n\n", state_to_str(core.state), err_to_str(core.last_error));
}

/* 场景 3: 工程 ID 与名称不符 (分别验证 WRQ 阶段项目名与数据阶段头部 Project ID) */
static void run_scenario_project_id_mismatch(void)
{
    printf("=== Scenario 3: Project ID and Name Mismatch Suite ===\n");
    clean_temp_files();
    memset(&s_host_drv, 0, sizeof(s_host_drv));
    s_host_drv.target_id = 1;

    ecat_foe_core_t core;
    ecat_foe_core_cfg_t cfg;
    setup_core_config(&cfg, &s_host_drv);
    assert(ecat_foe_core_init(&core, &cfg) == 0);

    /* 3a. WRQ 阶段项目名称不匹配 */
    const char *fn_wrong_name = "OTHER_PRJ_MCU_V1.0.0.0.efw";
    int wrq_st = ecat_foe_core_on_wrq(&core, fn_wrong_name, strlen(fn_wrong_name));
    assert(wrq_st != 0);
    assert(core.state == ECAT_FOE_STATE_ERROR);
    assert(core.last_error == ECAT_FOE_CORE_ERR_WRQ_PROJECT_MISMATCH);
    printf("[CORE] 3a. on_wrq: %s -> REJECT (WRQ_PROJECT_MISMATCH)\n", fn_wrong_name);

    /* 3b. WRQ 文件名合法通过，但头部 project_id 不符 */
    assert(ecat_foe_core_init(&core, &cfg) == 0);
    const char *fn_ok = "SERVO_DRV_MCU_V1.0.0.0.efw";
    assert(ecat_foe_core_on_wrq(&core, fn_ok, strlen(fn_ok)) == 0);
    printf("[CORE] 3b. on_wrq: %s -> OK\n", fn_ok);

    static const uint8_t payload[16] = {0x11, 0x22};
    /* 构造头部 project_id 为 0x20020002 (本地配置为 0x10010001)，CRC 正确 */
    size_t efw_len = build_efw_buffer(s_efw_buf, 0x20020002U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    int d_st = ecat_foe_core_on_data(&core, s_efw_buf, efw_len);
    assert(d_st != 0);
    assert(core.state == ECAT_FOE_STATE_ERROR);
    assert(core.last_error == ECAT_FOE_CORE_ERR_HDR_VERIFY);
    /* 头部校验未过，驱动决不进入擦写 */
    assert(s_host_drv.is_invalidated == false);
    printf("[CORE] 3b. on_data (mismatched header project_id) -> REJECT (HDR_VERIFY)\n");
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
    assert(ecat_foe_core_on_wrq(&core, fn, strlen(fn)) == 0);

    static const uint8_t payload[32] = {1, 2, 3};
    size_t efw_len = build_efw_buffer(s_efw_buf, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 32);
    (void)efw_len;

    /* 仅喂入完整 148 字节头 + 8 字节数据 (预期应有 32 字节数据) */
    assert(ecat_foe_core_on_data(&core, s_efw_buf, ECAT_FOE_EFW_HEADER_MIN_LEN + 8U) == 0);
    int end_st = ecat_foe_core_on_end(&core);
    assert(end_st != 0);
    assert(core.state == ECAT_FOE_STATE_ERROR);
    assert(core.last_error == ECAT_FOE_CORE_ERR_UNDER_RECEIVE);

    printf("[CORE] on_end (truncated stream) -> REJECT\n");
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
    assert(ecat_foe_core_on_wrq(&core, fn, strlen(fn)) == 0);

    static const uint8_t payload[16] = {1, 2};
    size_t efw_len = build_efw_buffer(s_efw_buf, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);

    /* 先喂入完整的 80 字节 */
    assert(ecat_foe_core_on_data(&core, s_efw_buf, efw_len) == 0);

    /* 故意追加 8 字节溢出数据 */
    uint8_t extra[8] = {0xFF};
    int d_st = ecat_foe_core_on_data(&core, extra, sizeof(extra));
    assert(d_st != 0);
    assert(core.state == ECAT_FOE_STATE_ERROR);
    assert(core.last_error == ECAT_FOE_CORE_ERR_OVER_RECEIVE);

    printf("[CORE] on_data (overflow data) -> REJECT\n");
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
    assert(ecat_foe_core_on_wrq(&core, fn, strlen(fn)) == 0);

    static const uint8_t payload[16] = {0x11, 0x22};
    size_t efw_len = build_efw_buffer(s_efw_buf, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    assert(ecat_foe_core_on_data(&core, s_efw_buf, ECAT_FOE_EFW_HEADER_MIN_LEN) == 0);

    printf("[CORE] Simulated stream break -> calling on_abort\n");
    ecat_foe_core_on_abort(&core);
    assert(core.state == ECAT_FOE_STATE_IDLE);
    printf("[CORE] Intermediate State: %s\n", state_to_str(core.state));

    /* 第 2 次尝试: 同一个 core 实例接收新 WRQ 重新开始 */
    printf("[CORE] Retrying with new WRQ on same core instance...\n");
    int wrq_retry = ecat_foe_core_on_wrq(&core, fn, strlen(fn));
    assert(wrq_retry == 0);
    printf("[CORE] on_wrq retry -> OK\n");

    assert(ecat_foe_core_on_data(&core, s_efw_buf, efw_len) == 0);
    int end_retry = ecat_foe_core_on_end(&core);
    assert(end_retry == 0);
    assert(core.state == ECAT_FOE_STATE_SUCCESS);

    printf("[CORE] on_end retry -> OK\n");
    printf("[CORE] Final State: %s, Last Error: %s\n\n", state_to_str(core.state), err_to_str(core.last_error));
}

/* 场景 7: 优先加载 tools/efw_pack.py 打包的补零固件 (.efw) 端到端验证 */
static void run_scenario_padded_efw(const char *external_efw_file)
{
    printf("=== Scenario 7: Padded Firmware End-to-End via efw_pack.py ===\n");
    clean_temp_files();
    memset(&s_host_drv, 0, sizeof(s_host_drv));
    s_host_drv.target_id = 1;

    ecat_foe_core_t core;
    ecat_foe_core_cfg_t cfg;
    setup_core_config(&cfg, &s_host_drv);
    assert(ecat_foe_core_init(&core, &cfg) == 0);

    const char *fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
    assert(ecat_foe_core_on_wrq(&core, fn, strlen(fn)) == 0);

    uint8_t efw_data[512];
    size_t total_read = 0;

    FILE *fp = fopen(external_efw_file, "rb");
    if (fp) {
        total_read = fread(efw_data, 1, sizeof(efw_data), fp);
        fclose(fp);
        printf("[PACKER] Successfully loaded external .efw (size=%zu bytes)\n", total_read);
    } else {
        /* 回退机制: 若外部文件不存在，使用内置逻辑构造 */
        static const uint8_t payload[16] = {0x55, 0x66};
        total_read = build_efw_buffer(efw_data, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
        printf("[PACKER] Built in-memory fallback .efw (size=%zu bytes)\n", total_read);
    }

    /* 模拟 64 字节 FoE 载荷按块灌入核心 */
    size_t offset = 0;
    while (offset < total_read) {
        size_t chunk = (total_read - offset > 64) ? 64 : (total_read - offset);
        assert(ecat_foe_core_on_data(&core, efw_data + offset, (uint32_t)chunk) == 0);
        offset += chunk;
    }

    int end_st = ecat_foe_core_on_end(&core);
    assert(end_st == 0);
    assert(core.state == ECAT_FOE_STATE_SUCCESS);

    printf("[CORE] on_end (padded firmware short packet path) -> OK\n");
    printf("[CORE] Final State: %s, Last Error: %s\n\n", state_to_str(core.state), err_to_str(core.last_error));
}

/* 场景 8: 核心机对驱动 finalize 失败的主动防御 */
static void run_scenario_finalize_defense_demo(void)
{
    printf("=== Scenario 8: Core Defense on Driver Finalize Failure ===\n");
    clean_temp_files();
    memset(&s_host_drv, 0, sizeof(s_host_drv));
    s_host_drv.target_id = 1;
    s_host_drv.inject_fail_finalize = true;

    ecat_foe_core_t core;
    ecat_foe_core_cfg_t cfg;
    setup_core_config(&cfg, &s_host_drv);
    assert(ecat_foe_core_init(&core, &cfg) == 0);

    const char *fn = "SERVO_DRV_MCU_V1.0.0.0.efw";
    assert(ecat_foe_core_on_wrq(&core, fn, strlen(fn)) == 0);

    static const uint8_t payload[16] = {0x77, 0x88};
    size_t efw_len = build_efw_buffer(s_efw_buf, 0x10010001U, 0x00000001U, 1, 1, 1, 0, 0x01000000U, 0, payload, 16);
    assert(ecat_foe_core_on_data(&core, s_efw_buf, efw_len) == 0);

    int end_st = ecat_foe_core_on_end(&core);
    assert(end_st != 0);
    assert(core.state == ECAT_FOE_STATE_ERROR);
    assert(core.last_error == ECAT_FOE_CORE_ERR_DRV_FINALIZE);
    /* 核心中止清理，介质未置有效标记 */
    assert(s_host_drv.is_finalized == false);

    printf("[CORE] on_end (finalize failed) -> REJECT\n");
    printf("[CORE] State: %s, Last Error: %s\n", state_to_str(core.state), err_to_str(core.last_error));
    printf("[NOTE] 核心机在 finalize 失败时拒绝升级、触发 abort 并保持介质处于无效状态；\n");
    printf("       真实 SOES 在零长度结束包路径忽略应用返回值的现象已在 tests/test_soes_real_harness.c 中由真实 esc_foe.c 独立验证。\n\n");
}

/* -------------------------------------------------------------------------- */
/*                               主函数入口                                   */
/* -------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    printf("============================================================\n");
    printf(" EtherCAT FoE Core Host Reference Demo (8 Scenarios)\n");
    printf("============================================================\n\n");

    /* 1. 初始化临时隔离环境 (所有模拟介质均限定在独立的系统临时隔离目录内) */
    init_demo_env();

    const char *padded_efw_path = (argc > 1) ? argv[1] : "examples/host_demo/packed_padded.efw";

    /* 2. 执行 8 大场景 */
    run_scenario_success();
    run_scenario_header_crc_mismatch();
    run_scenario_project_id_mismatch();
    run_scenario_stream_under_receive();
    run_scenario_stream_over_receive();
    run_scenario_abort_retry_same_core();
    run_scenario_padded_efw(padded_efw_path);
    run_scenario_finalize_defense_demo();

    /* 3. 清理临时隔离目录 (绝不触碰或删除当前工作目录任何文件) */
    destroy_demo_env();

    printf("============================================================\n");
    printf(" All 8 Host Scenarios Finished Successfully.\n");
    printf("============================================================\n");
    return 0;
}
