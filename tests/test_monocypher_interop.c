/* Real CLI-to-core Ed25519ph integration; Monocypher is supplied externally.
 * 边界说明: 本测试链接外部固定 Monocypher 4.0.3 密码学库，验证真实私钥签名与端到端密码学公钥验签；
 * 常规单元测试 (如 test_core.c, test_transfer_bidirectional.c) 使用 mock_signature 回调验证状态机与生命周期契约。
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ecat_foe_core.h"
#include "ecat_foe_monocypher_verifier.h"
#include "mock_driver.h"
#include "ecat_transfer.h"
#include "ecat_target_registry.h"
#include "ecat_foe_core_async.h"
#include "ecat_foe_target_async.h"

#define HEADER_SIZE ECAT_FOE_EFW_HEADER_MIN_LEN
#define BUFFER_SIZE 4096U

int port_get_version_floor(uint16_t target_id, uint32_t *floor_ver)
{
    (void)target_id;
    (void)floor_ver;
    return -1;
}

static size_t read_file(const char *path, uint8_t *buffer, size_t capacity)
{
    FILE *fp = fopen(path, "rb");
    assert(fp != NULL);
    size_t n = fread(buffer, 1, capacity, fp);
    assert(!ferror(fp));
    assert(fgetc(fp) == EOF && feof(fp));
    assert(fclose(fp) == 0);
    return n;
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void update_header_crc(uint8_t *package)
{
    put_u32(package + 144U, ecat_foe_crc32_calc(package, 144U));
}

static int receive(const uint8_t *package, size_t size,
                   const ecat_foe_trusted_key_t *key, mock_driver_t *driver,
                   bool corrupt_readback)
{
    ecat_foe_monocypher_verifier_t verifier = {0};
    ecat_foe_signature_verifier_t callbacks = {0};
    ecat_foe_monocypher_verifier_init(&verifier, key, 1U, &callbacks);
    ecat_foe_core_cfg_t cfg = {0};
    cfg.project_name = "INTEROP";
    cfg.project_id = 0x10010001U;
    cfg.hardware_id = 1U;
    cfg.local_hw_ver = 1U;
    cfg.max_header_size = HEADER_SIZE;
    cfg.signature_verify_enable = true;
    cfg.signature_verifier = callbacks;
    cfg.targets[0].enabled = true;
    cfg.targets[0].target_id = 1U;
    cfg.targets[0].max_payload_size = MOCK_DRIVER_MEM_SIZE;
    cfg.targets[0].write_unit = 8U;
    cfg.targets[0].driver = &driver->driver;
    cfg.targets[0].user_ctx = driver;
    ecat_foe_core_t core;
    assert(ecat_foe_core_init(&core, &cfg) == 0);
    const char *name = "INTEROP_MCU_V1.2.3.4.efw";
    assert(ecat_foe_core_on_wrq(&core, name, strlen(name)) == 0);
    driver->inject_corrupt_read = corrupt_readback;
    for (size_t pos = 0; pos < size;) {
        size_t n = (size - pos > 37U) ? 37U : size - pos;
        if (ecat_foe_core_on_data(&core, package + pos, n) != 0) return -1;
        pos += n;
    }
    return ecat_foe_core_on_end(&core);
}

/* -------------------------------------------------------------------------- */
/* 异步 Flash 驱动模拟器 (供真实 Monocypher 密码学跨层别名端到端测试)          */
/* -------------------------------------------------------------------------- */
typedef struct {
    uint8_t flash[BUFFER_SIZE];
    bool valid;
    uint32_t erased_watermark;
    unsigned int poll_pending_count;
    unsigned int remaining_pending;
    unsigned int starts;
    unsigned int polls;
    unsigned int abort_calls;
    unsigned int finalize_calls;
    bool inject_read_corruption;
} async_monocypher_flash_t;

static ecat_foe_target_async_result_t async_mono_flash_poll(void *ctx, ecat_foe_target_async_op_t *op);

static ecat_foe_target_async_result_t async_mono_flash_start(void *ctx, ecat_foe_target_async_op_t *op)
{
    async_monocypher_flash_t *f = (async_monocypher_flash_t *)ctx;
    f->starts++;
    f->remaining_pending = f->poll_pending_count;
    if (f->remaining_pending > 0U) {
        return ECAT_FOE_TARGET_ASYNC_PENDING;
    }
    return async_mono_flash_poll(ctx, op);
}

static ecat_foe_target_async_result_t async_mono_flash_poll(void *ctx, ecat_foe_target_async_op_t *op)
{
    async_monocypher_flash_t *f = (async_monocypher_flash_t *)ctx;
    f->polls++;
    if (f->remaining_pending > 0U) {
        f->remaining_pending--;
        return ECAT_FOE_TARGET_ASYNC_PENDING;
    }
    switch (op->kind) {
    case ECAT_FOE_TARGET_OP_INIT:
        f->erased_watermark = 0U;
        break;
    case ECAT_FOE_TARGET_OP_INVALIDATE:
        f->valid = false;
        break;
    case ECAT_FOE_TARGET_OP_ERASE_TO: {
        uint32_t end = (op->need_end + 7U) & ~7U;
        if (end > sizeof(f->flash)) return ECAT_FOE_TARGET_ASYNC_FAILED;
        if (end > f->erased_watermark) {
            memset(f->flash + f->erased_watermark, 0xFF, end - f->erased_watermark);
            f->erased_watermark = end;
        }
        op->erased_end = end;
        break;
    }
    case ECAT_FOE_TARGET_OP_WRITE:
        if (op->offset > sizeof(f->flash) || op->length > sizeof(f->flash) - op->offset)
            return ECAT_FOE_TARGET_ASYNC_FAILED;
        memcpy(f->flash + op->offset, op->data, op->length);
        break;
    case ECAT_FOE_TARGET_OP_READ:
        if (op->offset > sizeof(f->flash) || op->length > sizeof(f->flash) - op->offset)
            return ECAT_FOE_TARGET_ASYNC_FAILED;
        memcpy(op->data, f->flash + op->offset, op->length);
        if (f->inject_read_corruption && op->length > 0U) {
            op->data[0] ^= 0x55U; /* 破坏回读数据，破坏 SHA-512 / 验签散列 */
        }
        break;
    case ECAT_FOE_TARGET_OP_FINALIZE:
        f->valid = true;
        f->finalize_calls++;
        break;
    default:
        break;
    }
    return ECAT_FOE_TARGET_ASYNC_COMPLETE;
}

static void async_mono_flash_abort(void *ctx)
{
    async_monocypher_flash_t *f = (async_monocypher_flash_t *)ctx;
    f->abort_calls++;
}

static void async_mono_flash_abort_session(void *ctx)
{
    async_monocypher_flash_t *f = (async_monocypher_flash_t *)ctx;
    f->valid = false;
    f->abort_calls++;
}

static const ecat_foe_target_async_ops_t s_async_mono_flash_ops = {
    async_mono_flash_start,
    async_mono_flash_poll,
    async_mono_flash_abort,
    async_mono_flash_abort_session
};

static void test_one_cross_layer_alias_case(
    const uint8_t *efw_package,
    size_t efw_size,
    const ecat_foe_trusted_key_t *key,
    uint8_t *transfer_staging_buf,
    size_t transfer_staging_len,
    uint8_t *core_packet_buf,
    size_t core_packet_len,
    unsigned int poll_pending_count,
    bool inject_read_corruption,
    bool tamper_sig,
    bool expect_success)
{
    uint8_t send_buf[BUFFER_SIZE];
    assert(efw_size <= sizeof(send_buf));
    memcpy(send_buf, efw_package, efw_size);
    if (tamper_sig) {
        send_buf[80] ^= 1U;          /* 翻转签名中的 1 bit */
        update_header_crc(send_buf); /* 重新计算 header_crc，使头部检验通过，在 finish 时由 Ed25519ph 检出 */
    }

    static async_monocypher_flash_t flash;
    memset(&flash, 0, sizeof(flash));
    flash.poll_pending_count = poll_pending_count;
    flash.inject_read_corruption = inject_read_corruption;

    static mock_driver_t fallback_driver;
    mock_driver_init(&fallback_driver);

    ecat_foe_monocypher_verifier_t verifier = {0};
    ecat_foe_signature_verifier_t callbacks = {0};
    ecat_foe_monocypher_verifier_init(&verifier, key, 1U, &callbacks);

    ecat_foe_core_cfg_t cfg = {0};
    cfg.project_name = "INTEROP";
    cfg.project_id = 0x10010001U;
    cfg.hardware_id = 1U;
    cfg.local_hw_ver = 1U;
    cfg.max_header_size = HEADER_SIZE;
    cfg.signature_verify_enable = true; /* 关键：真实开启密码学 Ed25519ph 验证 */
    cfg.signature_verifier = callbacks;
    cfg.targets[0].enabled = true;
    cfg.targets[0].target_id = 1U;
    cfg.targets[0].max_payload_size = sizeof(flash.flash);
    cfg.targets[0].write_unit = 8U;
    cfg.targets[0].asynchronous = true;
    cfg.targets[0].async_ops = &s_async_mono_flash_ops;
    cfg.targets[0].async_user_ctx = &flash;
    cfg.targets[0].driver = &fallback_driver.driver;
    cfg.targets[0].user_ctx = &fallback_driver;

    static ecat_foe_core_t core;
    ecat_foe_core_on_abort(&core);
    core.shared_session = NULL;
    assert(ecat_foe_core_init(&core, &cfg) == 0);

    static ecat_target_registry_t reg;
    assert(ecat_target_registry_init(&reg) == 0);
    ecat_target_desc_t fw_target = {0};
    fw_target.target_id = 1U;
    strncpy(fw_target.name, "firmware_mcu", sizeof(fw_target.name));
    fw_target.enabled = true;
    fw_target.type = ECAT_TARGET_TYPE_FIRMWARE;
    fw_target.permissions = ECAT_PERM_WRITE;
    fw_target.max_size = sizeof(flash.flash);
    fw_target.asynchronous = true;
    fw_target.user_ctx = &fallback_driver;
    fw_target.driver.io = fallback_driver.driver;
    assert(ecat_target_registry_add(&reg, &fw_target) == 0);

    static ecat_transfer_t mgr;
    assert(ecat_transfer_init(&mgr, &reg, &core) == 0);
    assert(ecat_transfer_set_staging_buffer(&mgr, transfer_staging_buf, transfer_staging_len) == 0);

    static uint8_t op_buf[256];
    static ecat_foe_core_async_t core_sess;
    assert(ecat_foe_core_async_init(&core_sess, &core, core_packet_buf, core_packet_len,
                                    op_buf, sizeof(op_buf)) == 0);
    assert(ecat_transfer_init_async_firmware(&mgr, ecat_foe_core_async_ops(), &core_sess) == 0);

    ecat_transfer_request_t req = {
        .transport_id = 1U,
        .target_id = 1U,
        .direction = ECAT_TRANSFER_DIR_IMPORT,
        .expected_size = (uint32_t)efw_size,
        .fw_version = 0x01020304U
    };
    const char *owner = "test_alias_real_crypto";
    uint64_t token = 0;
    assert(ecat_transfer_begin(&mgr, owner, &req, &token) == ECAT_TRANSFER_COMPLETE);

    for (size_t pos = 0; pos < efw_size;) {
        size_t chunk = (efw_size - pos > 37U) ? 37U : efw_size - pos;
        ecat_transfer_result_t wres = ecat_transfer_write(&mgr, owner, token, send_buf + pos, chunk);
        while (wres == ECAT_TRANSFER_PENDING) {
            wres = ecat_transfer_poll(&mgr, owner, token);
        }
        if (wres != ECAT_TRANSFER_COMPLETE && wres != ECAT_TRANSFER_CONTINUE) {
            assert(!expect_success);
            ecat_transfer_cancel(&mgr, owner, token);
            assert(flash.valid == false);
            assert(flash.finalize_calls == 0U);
            assert(flash.abort_calls >= 1U);
            return;
        }
        pos += chunk;
    }

    ecat_transfer_result_t fres = ecat_transfer_finish(&mgr, owner, token);
    while (fres == ECAT_TRANSFER_PENDING) {
        fres = ecat_transfer_poll(&mgr, owner, token);
    }
    if (expect_success) {
        assert(fres == ECAT_TRANSFER_COMPLETE);
        /* 关键断言 1: 真实 Monocypher Ed25519ph 验签成功并由核心触发 finalize 置位 valid */
        assert(flash.valid == true);
        assert(flash.finalize_calls == 1U);
        /* 关键断言 2: 逐字节比对写入 Flash 的 payload 与签名包完全一致 */
        assert(memcmp(flash.flash, efw_package + HEADER_SIZE, efw_size - HEADER_SIZE) == 0);
        /* 关键断言 3: 传输层 CRC 与预期一致 */
        assert(ecat_transfer_get_crc32(&mgr) == ecat_foe_crc32_calc(efw_package, efw_size));
    } else {
        /* 篡改用例: 必须严格拒绝，禁止 finalize，禁止置 valid，且必须安全调用 abort */
        assert(fres == ECAT_TRANSFER_FAILED);
        assert(flash.valid == false);
        assert(flash.finalize_calls == 0U);
        assert(flash.abort_calls >= 1U);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    uint8_t original[BUFFER_SIZE], package[BUFFER_SIZE], public_key[32];
    size_t size = read_file(argv[1], original, sizeof(original));
    size_t unsigned_size = read_file(argv[2], package, sizeof(package));
    assert(size > HEADER_SIZE && unsigned_size > HEADER_SIZE);
    assert(read_file(argv[3], public_key, sizeof(public_key)) == sizeof(public_key));
    ecat_foe_trusted_key_t key;
    memcpy(key.key_id, original + 72U, sizeof(key.key_id));
    memcpy(key.public_key, public_key, sizeof(key.public_key));
    static mock_driver_t driver;

    mock_driver_init(&driver);
    assert(receive(original, size, &key, &driver, false) == 0);
    assert(driver.count_finalize == 1U && driver.valid_flag);
    assert(memcmp(driver.mem, original + HEADER_SIZE, size - HEADER_SIZE) == 0);

    mock_driver_init(&driver);
    assert(receive(package, unsigned_size, &key, &driver, false) != 0);
    assert(driver.count_init == 0U && driver.count_finalize == 0U);

    memcpy(package, original, size);
    package[72] ^= 1U;
    update_header_crc(package);
    mock_driver_init(&driver);
    assert(receive(package, size, &key, &driver, false) != 0);
    assert(driver.count_init == 0U && driver.count_finalize == 0U);

    memcpy(package, original, size);
    package[80] ^= 1U;
    update_header_crc(package);
    mock_driver_init(&driver);
    assert(receive(package, size, &key, &driver, false) != 0);
    assert(driver.count_abort == 1U && driver.count_finalize == 0U && !driver.valid_flag);

    memcpy(package, original, size);
    package[18] = 2U; /* Still compatible hardware range, but signed metadata changed. */
    update_header_crc(package);
    mock_driver_init(&driver);
    assert(receive(package, size, &key, &driver, false) != 0);
    assert(driver.count_abort == 1U && driver.count_finalize == 0U && !driver.valid_flag);

    memcpy(package, original, size);
    package[HEADER_SIZE] ^= 1U;
    put_u32(package + 36U, ecat_foe_crc32_calc(package + HEADER_SIZE, size - HEADER_SIZE));
    update_header_crc(package);
    mock_driver_init(&driver);
    assert(receive(package, size, &key, &driver, false) != 0);
    assert(driver.count_abort == 1U && driver.count_finalize == 0U && !driver.valid_flag);

    mock_driver_init(&driver);
    assert(receive(original, size, &key, &driver, true) != 0);
    assert(driver.count_abort == 1U && driver.count_finalize == 0U && !driver.valid_flag);

    /* ====================================================================== */
    /* 基于真实 Monocypher 4.0.3 Ed25519ph 的跨层别名与篡改拒绝端到端测试     */
    /* ====================================================================== */
    static uint8_t cross_backing[512];
    static uint8_t independent_core_packet[256];

    /* 1. 拓扑 1: 前向部分重叠 (staging = &cross[0], packet = &cross[16]) */
    test_one_cross_layer_alias_case(original, size, &key, &cross_backing[0], 256,
                                    &cross_backing[16], 256, 0U, false, false, true);
    test_one_cross_layer_alias_case(original, size, &key, &cross_backing[0], 256,
                                    &cross_backing[16], 256, 1U, false, false, true);

    /* 2. 拓扑 2: 后向部分重叠 (staging = &cross[16], packet = &cross[0]) */
    test_one_cross_layer_alias_case(original, size, &key, &cross_backing[16], 256,
                                    &cross_backing[0], 256, 0U, false, false, true);
    test_one_cross_layer_alias_case(original, size, &key, &cross_backing[16], 256,
                                    &cross_backing[0], 256, 1U, false, false, true);

    /* 3. 拓扑 3: 完全相同地址 (staging = &cross[0], packet = &cross[0]) */
    test_one_cross_layer_alias_case(original, size, &key, &cross_backing[0], 256,
                                    &cross_backing[0], 256, 0U, false, false, true);
    test_one_cross_layer_alias_case(original, size, &key, &cross_backing[0], 256,
                                    &cross_backing[0], 256, 1U, false, false, true);

    /* 4. 拓扑 4: 独立静态缓冲 (staging = &cross[0], packet = &independent[0]) */
    test_one_cross_layer_alias_case(original, size, &key, &cross_backing[0], 256,
                                    &independent_core_packet[0], 256, 0U, false, false, true);
    test_one_cross_layer_alias_case(original, size, &key, &cross_backing[0], 256,
                                    &independent_core_packet[0], 256, 1U, false, false, true);

    /* 5. 篡改拒绝与安全释放 (Tampering & Corrupt Readback Rejection) */
    /* 5.1 签名篡改: 翻转签名 1 bit，真实 Ed25519ph 验签拒绝 */
    test_one_cross_layer_alias_case(original, size, &key, &cross_backing[0], 256,
                                    &cross_backing[16], 256, 0U, false, true, false);
    test_one_cross_layer_alias_case(original, size, &key, &cross_backing[0], 256,
                                    &cross_backing[16], 256, 1U, false, true, false);

    /* 5.2 载荷/回读损坏: Flash 回读数据损坏，SHA-512 散列不符拒绝 */
    test_one_cross_layer_alias_case(original, size, &key, &cross_backing[0], 256,
                                    &cross_backing[16], 256, 0U, true, false, false);
    test_one_cross_layer_alias_case(original, size, &key, &cross_backing[0], 256,
                                    &cross_backing[16], 256, 1U, true, false, false);

    /* 6. 核心内部两缓冲自身重叠显式拒绝用例 (Core Internal Overlap Rejection) */
    {
        static ecat_foe_core_t reject_core;
        ecat_foe_core_cfg_t reject_cfg = {0};
        reject_cfg.project_name = "REJECT";
        reject_cfg.hardware_id = 1U;
        reject_cfg.local_hw_ver = 1U;
        reject_cfg.max_header_size = HEADER_SIZE;
        reject_cfg.targets[0].enabled = true;
        reject_cfg.targets[0].max_payload_size = 1024U;
        reject_cfg.targets[0].write_unit = 8U;
        static mock_driver_t rej_drv;
        mock_driver_init(&rej_drv);
        reject_cfg.targets[0].driver = &rej_drv.driver;
        assert(ecat_foe_core_init(&reject_core, &reject_cfg) == 0);

        static ecat_foe_core_async_t rej_sess;
        static uint8_t buf[512];
        /* 尝试将 packet (buf[0..255]) 与 op_staging (buf[16..143]) 交叠传给核心初始化 */
        int rc = ecat_foe_core_async_init(&rej_sess, &reject_core, &buf[0], 256, &buf[16], 128);
        assert(rc == -1); /* 核心内部防御生效，显式拒绝整段交叠 */
    }

    puts("PASS: CLI/Monocypher/core interop, cross-layer alias topologies, and real crypto tampering rejection");
    return 0;
}
