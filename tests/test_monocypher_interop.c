#include "test_core_helpers.h"
/* Real CLI-to-core Ed25519ph integration; Monocypher is supplied externally.
 * 边界说明: 本测试链接外部固定 Monocypher 4.0.3 密码学库，验证真实私钥签名与端到端密码学公钥验签；
 * 常规单元测试 (如 test_core.c, test_transfer_bidirectional.c) 使用 mock_signature 回调验证状态机与生命周期契约。
 *
 * 轮询上限: 所有 while(PENDING) 循环均设 MAX_POLL_ITERS 次数上限，超时即断言失败，防止 CI 挂起。
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

/* 轮询保护: 防止 CI 在驱动回归时因 PENDING 无限循环而挂起 */
#define MAX_POLL_ITERS 1000

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

static int receive_for_target(const uint8_t *package, size_t size,
                              const ecat_foe_trusted_key_t *key, mock_driver_t *driver,
                              bool corrupt_readback, uint16_t target_id,
                              const char *foe_name)
{
    ecat_foe_monocypher_verifier_t verifier = {0};
    ecat_foe_signature_verifier_t callbacks = {0};
    ecat_foe_monocypher_verifier_init(&verifier, key, 1U, &callbacks);

    ecat_target_registry_t reg;
    ecat_target_desc_t target = {0};
    assert(ecat_target_registry_init(&reg) == 0);
    target.target_id = target_id;
    memcpy(target.name, "fw_slot", sizeof("fw_slot"));
    target.type = ECAT_TARGET_TYPE_FIRMWARE;
    target.permissions = ECAT_PERM_WRITE;
    target.max_size = MOCK_DRIVER_MEM_SIZE;
    target.write_unit = 8U;
    target.enabled = true;
    target.driver.io = driver->driver;
    target.user_ctx = driver;
    assert(ecat_target_registry_add(&reg, &target) == 0);

    ecat_foe_core_cfg_t cfg = {0};
    cfg.project_name = "INTEROP";
    cfg.project_id = 0x10010001U;
    cfg.hardware_id = 1U;
    cfg.local_hw_ver = 1U;
    cfg.max_header_size = HEADER_SIZE;
    cfg.signature_verify_enable = true;
    cfg.signature_verifier = callbacks;
    cfg.registry = &reg;
    ecat_foe_core_t core;
    assert(ecat_foe_core_init(&core, &cfg) == 0);
    assert(test_core_begin_filename(&core, foe_name, strlen(foe_name)) == 0);
    driver->inject_corrupt_read = corrupt_readback;
    for (size_t pos = 0; pos < size;) {
        size_t n = (size - pos > 37U) ? 37U : size - pos;
        if (ecat_foe_core_on_data(&core, package + pos, n) != 0) return -1;
        pos += n;
    }
    return ecat_foe_core_on_end(&core);
}

static int receive(const uint8_t *package, size_t size,
                   const ecat_foe_trusted_key_t *key, mock_driver_t *driver,
                   bool corrupt_readback)
{
    return receive_for_target(package, size, key, driver, corrupt_readback,
                              1U, "INTEROP_MCU_V1.2.3.4.efw");
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
    const ecat_foe_core_t *core;               /* 用于在清理前捕获核心机中止错误 */
    ecat_foe_core_err_t observed_abort_error;  /* 记录驱动中止时核心上报告的细分错误 */
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
            op->data[0] ^= 0x55U; /* 破坏回读数据，触发物理介质回读 CRC32 校验失败 (READBACK_CRC_MISMATCH) */
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
    if (f->core != NULL && f->core->last_error != ECAT_FOE_CORE_OK && f->observed_abort_error == ECAT_FOE_CORE_OK) {
        f->observed_abort_error = f->core->last_error;
    }
}

static void async_mono_flash_abort_session(void *ctx)
{
    async_monocypher_flash_t *f = (async_monocypher_flash_t *)ctx;
    f->valid = false;
    f->abort_calls++;
    if (f->core != NULL && f->core->last_error != ECAT_FOE_CORE_OK && f->observed_abort_error == ECAT_FOE_CORE_OK) {
        f->observed_abort_error = f->core->last_error;
    }
}

static const ecat_foe_target_async_ops_t s_async_mono_flash_ops = {
    async_mono_flash_start,
    async_mono_flash_poll,
    async_mono_flash_abort,
    async_mono_flash_abort_session
};

/* -------------------------------------------------------------------------- */
/* 可观察验签包装器 (Counted Verifier Wrapper)                                 */
/* 严格包装生产 examples/crypto/ecat_foe_monocypher_verifier.c，叠加调用计数，   */
/* 绝不复制加密算法逻辑，100% 委托给产品适配器与外部真实 Monocypher 库          */
/* -------------------------------------------------------------------------- */
typedef struct {
    ecat_foe_monocypher_verifier_t real_verifier; /* 真实生产验签器实例 */
    ecat_foe_signature_verifier_t real_callbacks; /* 真实生产回调函数表 */
    unsigned begin_calls;
    unsigned update_calls;
    unsigned finish_calls;
    unsigned abort_calls;
    int last_finish_rc;
} counted_verifier_wrapper_t;

static int counted_begin(const uint8_t *key_id, const uint8_t *signature, void *ctx)
{
    counted_verifier_wrapper_t *w = (counted_verifier_wrapper_t *)ctx;
    w->begin_calls++;
    return w->real_callbacks.begin(key_id, signature, w->real_callbacks.user_ctx);
}

static int counted_update(const uint8_t *data, size_t len, void *ctx)
{
    counted_verifier_wrapper_t *w = (counted_verifier_wrapper_t *)ctx;
    w->update_calls++;
    return w->real_callbacks.update(data, len, w->real_callbacks.user_ctx);
}

static int counted_finish(void *ctx)
{
    counted_verifier_wrapper_t *w = (counted_verifier_wrapper_t *)ctx;
    w->finish_calls++;
    w->last_finish_rc = w->real_callbacks.finish(w->real_callbacks.user_ctx);
    return w->last_finish_rc;
}

static void counted_abort(void *ctx)
{
    counted_verifier_wrapper_t *w = (counted_verifier_wrapper_t *)ctx;
    w->abort_calls++;
    if (w->real_callbacks.abort != NULL) {
        w->real_callbacks.abort(w->real_callbacks.user_ctx);
    }
}

static void counted_verifier_init(counted_verifier_wrapper_t *w,
                                  const ecat_foe_trusted_key_t *keys,
                                  size_t key_count,
                                  ecat_foe_signature_verifier_t *callbacks)
{
    memset(w, 0, sizeof(*w));
    /* 通过真实生产初始化接口填充真实回调表与验签器实例 */
    ecat_foe_monocypher_verifier_init(&w->real_verifier, keys, key_count, &w->real_callbacks);

    callbacks->begin    = counted_begin;
    callbacks->update   = counted_update;
    callbacks->finish   = counted_finish;
    callbacks->abort    = counted_abort;
    callbacks->user_ctx = w;
}

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

    counted_verifier_wrapper_t verifier;
    ecat_foe_signature_verifier_t callbacks = {0};
    counted_verifier_init(&verifier, key, 1U, &callbacks);

    static ecat_target_registry_t reg;
    assert(ecat_target_registry_init(&reg) == 0);
    ecat_target_desc_t fw_target = {0};
    fw_target.target_id = 1U;
    strncpy(fw_target.name, "firmware_mcu", sizeof(fw_target.name));
    fw_target.enabled = true;
    fw_target.type = ECAT_TARGET_TYPE_FIRMWARE;
    fw_target.permissions = ECAT_PERM_WRITE;
    fw_target.max_size = sizeof(flash.flash);
    fw_target.write_unit = 8U;
    fw_target.asynchronous = true;
    fw_target.user_ctx = &fallback_driver;
    fw_target.driver.io = fallback_driver.driver;
    fw_target.driver.fw_async_ops = &s_async_mono_flash_ops;
    fw_target.driver.fw_async_user_ctx = &flash;
    assert(ecat_target_registry_add(&reg, &fw_target) == 0);

    ecat_foe_core_cfg_t cfg = {0};
    cfg.project_name = "INTEROP";
    cfg.project_id = 0x10010001U;
    cfg.hardware_id = 1U;
    cfg.local_hw_ver = 1U;
    cfg.max_header_size = HEADER_SIZE;
    cfg.signature_verify_enable = true; /* 关键：真实开启密码学 Ed25519ph 验证 */
    cfg.signature_verifier = callbacks;
    cfg.registry = &reg;

    static ecat_foe_core_t core;
    ecat_foe_core_on_abort(&core);
    core.shared_session = NULL;
    assert(ecat_foe_core_init(&core, &cfg) == 0);
    flash.core = &core; /* 供驱动在 abort 发生瞬间捕获核心细分诊断 */

    static ecat_transfer_t mgr;
    assert(ecat_transfer_init(&mgr, &reg, &core) == 0);
    assert(ecat_transfer_set_staging_buffer(&mgr, transfer_staging_buf, transfer_staging_len) == 0);

    static uint8_t op_buf[256];
    static ecat_foe_core_async_t core_sess;
    assert(ecat_foe_core_async_init(&core_sess, &core, core_packet_buf, core_packet_len,
                                    op_buf, sizeof(op_buf)) == 0);
    assert(ecat_transfer_init_async_firmware(&mgr, &core_sess) == 0);

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
        int poll_iters = 0;
        while (wres == ECAT_TRANSFER_PENDING) {
            assert(++poll_iters < MAX_POLL_ITERS && "write poll exceeded limit");
            wres = ecat_transfer_poll(&mgr, owner, token);
        }
        /* 严禁提前退出：正向、签名篡改、回读损坏用例在写入阶段头部与分块均完整，写入阶段必须成功完成接收 */
        assert(wres == ECAT_TRANSFER_COMPLETE || wres == ECAT_TRANSFER_CONTINUE);
        pos += chunk;
    }

    ecat_transfer_result_t fres = ecat_transfer_finish(&mgr, owner, token);
    int finish_poll_iters = 0;
    while (fres == ECAT_TRANSFER_PENDING) {
        assert(++finish_poll_iters < MAX_POLL_ITERS && "finish poll exceeded limit");
        fres = ecat_transfer_poll(&mgr, owner, token);
    }
    if (expect_success) {
        assert(fres == ECAT_TRANSFER_COMPLETE);
        /* 关键断言 1: 真实 Monocypher Ed25519ph 验签成功并由核心触发 finalize 置位 valid */
        assert(flash.valid == true);
        assert(flash.finalize_calls == 1U);
        assert(verifier.begin_calls == 1U);
        assert(verifier.update_calls > 0U);
        assert(verifier.finish_calls == 1U);
        assert(verifier.last_finish_rc == 0); /* 证明真实产品 verifier_finish 执行并返回成功 (0) */
        assert(verifier.abort_calls == 0U);
        assert(verifier.real_verifier.active == false); /* 真实生产适配器验签上下文已完成并安全 wipe */
        /* 关键断言 2: 逐字节比对写入 Flash 的 payload 与签名包完全一致 */
        assert(memcmp(flash.flash, efw_package + HEADER_SIZE, efw_size - HEADER_SIZE) == 0);
        /* 关键断言 3: 传输层 CRC 与预期一致 */
        assert(ecat_transfer_get_crc32(&mgr) == ecat_foe_crc32_calc(efw_package, efw_size));
        assert(flash.observed_abort_error == ECAT_FOE_CORE_OK);
        assert(ecat_foe_core_get_last_error(&core) == ECAT_FOE_CORE_OK);
    } else if (tamper_sig) {
        /* 篡改签名用例: 写入与回读均顺利完成，真实到达 Monocypher finish，由真实产品适配器与 Ed25519ph 检出无效签名拒绝 */
        assert(fres == ECAT_TRANSFER_FAILED);
        assert(flash.valid == false);
        assert(flash.finalize_calls == 0U);
        assert(verifier.begin_calls == 1U);
        assert(verifier.update_calls > 0U);
        assert(verifier.finish_calls == 1U); /* 证明真正到达并执行了真实适配器的 Ed25519ph finish 密码学校验 */
        assert(verifier.last_finish_rc != 0); /* 证明真实产品 verifier_finish 执行并检出签名无效返回非 0 错误 */
        assert(verifier.real_verifier.active == false); /* 真实生产适配器验签上下文已安全 wipe */
        assert(flash.observed_abort_error == ECAT_FOE_CORE_ERR_SIGNATURE_INVALID);
        assert(ecat_foe_core_get_last_error(&core) == ECAT_FOE_CORE_ERR_SIGNATURE_INVALID);
        assert(flash.abort_calls >= 1U);
    } else if (inject_read_corruption) {
        /* 回读损坏用例: 在 finish 阶段回读比对时，由物理介质回读 CRC32 完整性校验优先拦截 */
        assert(fres == ECAT_TRANSFER_FAILED);
        assert(flash.valid == false);
        assert(flash.finalize_calls == 0U);
        assert(verifier.begin_calls == 1U);
        assert(verifier.update_calls > 0U);
        assert(verifier.finish_calls == 0U); /* 因回读 CRC 优先报错，尚未进入 finish 密码学校验 */
        assert(verifier.abort_calls >= 1U);   /* 失败时安全中止并清理真实验签会话 */
        assert(verifier.real_verifier.active == false);
        assert(flash.observed_abort_error == ECAT_FOE_CORE_ERR_READBACK_CRC_MISMATCH);
        assert(ecat_foe_core_get_last_error(&core) == ECAT_FOE_CORE_ERR_READBACK_CRC_MISMATCH);
        assert(flash.abort_calls >= 1U);
    } else {
        assert(0 && "unhandled test branch");
    }

    /* 验证会话租约与状态已完全释放：无论成功还是失败，新所有者均可重新获得会话 */
    const char *new_owner = "test_alias_probe_owner";
    uint64_t new_token = 0;
    ecat_transfer_request_t probe_req = req;
    probe_req.direction = ECAT_TRANSFER_DIR_IMPORT;
    ecat_transfer_result_t probe_res = ecat_transfer_begin(&mgr, new_owner, &probe_req, &new_token);
    assert(probe_res == ECAT_TRANSFER_COMPLETE || probe_res == ECAT_TRANSFER_PENDING);
    ecat_transfer_cancel(&mgr, new_owner, new_token);
}

int main(int argc, char **argv)
{
    assert(argc == 5);
    uint8_t original[BUFFER_SIZE], package[BUFFER_SIZE], ext_signed[BUFFER_SIZE], public_key[32];
    size_t size = read_file(argv[1], original, sizeof(original));
    size_t unsigned_size = read_file(argv[2], package, sizeof(package));
    size_t ext_size = read_file(argv[4], ext_signed, sizeof(ext_signed));
    assert(size > HEADER_SIZE && unsigned_size > HEADER_SIZE && ext_size > HEADER_SIZE);
    assert(read_file(argv[3], public_key, sizeof(public_key)) == sizeof(public_key));
    ecat_foe_trusted_key_t key;
    memcpy(key.key_id, original + 72U, sizeof(key.key_id));
    memcpy(key.public_key, public_key, sizeof(key.public_key));
    static mock_driver_t driver;

    mock_driver_init(&driver);
    assert(receive(original, size, &key, &driver, false) == 0);
    assert(driver.count_finalize == 1U && driver.valid_flag);
    assert(memcmp(driver.mem, original + HEADER_SIZE, size - HEADER_SIZE) == 0);

    /* 扩展目标 T002A (42) 真实 CLI 签名包验签通过，且跨目标重放至 MCU (1) 在驱动调用前被拒绝 */
    mock_driver_init(&driver);
    assert(receive_for_target(ext_signed, ext_size, &key, &driver, false,
                              42U, "INTEROP_T002A_V1.2.3.4.efw") == 0);
    assert(driver.count_finalize == 1U && driver.valid_flag);
    assert(memcmp(driver.mem, ext_signed + HEADER_SIZE, ext_size - HEADER_SIZE) == 0);

    mock_driver_init(&driver);
    assert(receive(ext_signed, ext_size, &key, &driver, false) != 0);
    assert(driver.count_init == 0U && driver.count_finalize == 0U);

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

    /* 5.2 载荷/回读损坏: Flash 回读数据损坏，回读 CRC32 完整性校验拦截 */
    test_one_cross_layer_alias_case(original, size, &key, &cross_backing[0], 256,
                                    &cross_backing[16], 256, 0U, true, false, false);
    test_one_cross_layer_alias_case(original, size, &key, &cross_backing[0], 256,
                                    &cross_backing[16], 256, 1U, true, false, false);

    /* 6. 核心内部两缓冲自身重叠显式拒绝用例 (Core Internal Overlap Rejection) */
    {
        static ecat_foe_core_t reject_core;
        static ecat_target_registry_t reject_reg;
        static mock_driver_t rej_drv;
        mock_driver_init(&rej_drv);
        assert(ecat_target_registry_init(&reject_reg) == 0);
        ecat_target_desc_t rej_target = {0};
        rej_target.target_id = 1U;
        memcpy(rej_target.name, "mcu", sizeof("mcu"));
        rej_target.type = ECAT_TARGET_TYPE_FIRMWARE;
        rej_target.permissions = ECAT_PERM_WRITE;
        rej_target.max_size = 1024U;
        rej_target.write_unit = 8U;
        rej_target.enabled = true;
        rej_target.driver.io = rej_drv.driver;
        rej_target.user_ctx = &rej_drv;
        assert(ecat_target_registry_add(&reject_reg, &rej_target) == 0);

        ecat_foe_core_cfg_t reject_cfg = {0};
        reject_cfg.project_name = "REJECT";
        reject_cfg.hardware_id = 1U;
        reject_cfg.local_hw_ver = 1U;
        reject_cfg.max_header_size = HEADER_SIZE;
        reject_cfg.registry = &reject_reg;
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
