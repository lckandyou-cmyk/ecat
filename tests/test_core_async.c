#include "test_core_helpers.h"
#include "ecat_foe_core_async.h"
#include "ecat_foe_ssc.h"
#include "mock_driver.h"
#include "ecat_transfer_file.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

typedef struct {
    uint8_t flash[4096];
    bool pending;
    bool valid;
    unsigned starts;
    unsigned polls;
    unsigned aborts;
    uint32_t erased_watermark;
    bool immediate;
    bool fail_init_immediate;
    bool fail_init_poll;
    bool fail_invalidate;
    bool fail_finalize;
    uint16_t expected_target_id;
    bool resource_acquired;
    bool safe_session_abort;
} async_flash_t;

typedef struct {
    unsigned begin_calls;
    unsigned update_calls;
    unsigned finish_calls;
    unsigned abort_calls;
} test_sig_verifier_t;

static int test_sig_begin(const uint8_t key_id[ECAT_FOE_EFW_KEY_ID_LEN],
                          const uint8_t signature[ECAT_FOE_EFW_SIGNATURE_LEN],
                          void *user_ctx)
{
    (void)key_id;
    (void)signature;
    test_sig_verifier_t *v = (test_sig_verifier_t *)user_ctx;
    v->begin_calls++;
    return 0;
}

static int test_sig_update(const uint8_t *data, size_t len, void *user_ctx)
{
    (void)data;
    (void)len;
    test_sig_verifier_t *v = (test_sig_verifier_t *)user_ctx;
    v->update_calls++;
    return 0;
}

static int test_sig_finish(void *user_ctx)
{
    test_sig_verifier_t *v = (test_sig_verifier_t *)user_ctx;
    v->finish_calls++;
    return 0;
}

static void test_sig_abort(void *user_ctx)
{
    test_sig_verifier_t *v = (test_sig_verifier_t *)user_ctx;
    v->abort_calls++;
}

static ecat_foe_target_async_result_t flash_poll(void *ctx, ecat_foe_target_async_op_t *op);

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static size_t make_efw(uint8_t *out, const uint8_t *payload, uint32_t length)
{
    memset(out, 0, ECAT_FOE_EFW_HEADER_MIN_LEN + length);
    put32(out, ECAT_FOE_EFW_MAGIC);
    put16(out + 4, ECAT_FOE_EFW_FORMAT_VERSION);
    put16(out + 6, ECAT_FOE_EFW_HEADER_MIN_LEN);
    put32(out + 8, 0x10010001U);
    put32(out + 12, 1U);
    put16(out + 16, 1U);
    put16(out + 18, 2U);
    put16(out + 20, ECAT_FOE_TARGET_MCU);
    put32(out + 24, 0x01000000U);
    put32(out + 28, 0U);
    put32(out + 32, length);
    put32(out + 36, ecat_foe_crc32_calc(payload, length));
    memcpy(out + 60, "SIG2", 4U);
    out[64] = 1U;
    put32(out + 144, ecat_foe_crc32_calc(out, 144U));
    memcpy(out + ECAT_FOE_EFW_HEADER_MIN_LEN, payload, length);
    return ECAT_FOE_EFW_HEADER_MIN_LEN + length;
}

static size_t make_signed_efw(uint8_t *out, const uint8_t *payload, uint32_t length)
{
    size_t total = make_efw(out, payload, length);
    put16(out + 66, 1U);
    put16(out + 68, 64U);
    put16(out + 70, 8U);
    for (size_t i = 0; i < 8; i++) out[72 + i] = (uint8_t)(0x10U + i);
    memset(out + 80, 0xA5, 64);
    put32(out + 144, ecat_foe_crc32_calc(out, 144U));
    return total;
}

static ecat_foe_target_async_result_t flash_start(void *ctx,
                                                   ecat_foe_target_async_op_t *op)
{
    async_flash_t *f = (async_flash_t *)ctx;
    f->starts++;
    f->pending = true;
    f->resource_acquired = true;
    if (op->kind == ECAT_FOE_TARGET_OP_INIT && f->fail_init_immediate) {
        if (!f->safe_session_abort) {
            /* 模拟旧版驱动契约：INIT 立即失败由驱动在 start 内部自行回滚资源 */
            f->resource_acquired = false;
            f->pending = false;
        }
        return ECAT_FOE_TARGET_ASYNC_FAILED;
    }
    if (f->immediate) return flash_poll(ctx, op);
    return ECAT_FOE_TARGET_ASYNC_PENDING;
}

static ecat_foe_target_async_result_t flash_poll(void *ctx,
                                                  ecat_foe_target_async_op_t *op)
{
    async_flash_t *f = (async_flash_t *)ctx;
    f->polls++;
    if (op->kind == ECAT_FOE_TARGET_OP_INIT && f->fail_init_poll) {
        return ECAT_FOE_TARGET_ASYNC_FAILED;
    }
    if (op->kind == ECAT_FOE_TARGET_OP_INVALIDATE && f->fail_invalidate) {
        return ECAT_FOE_TARGET_ASYNC_FAILED;
    }
    switch (op->kind) {
    case ECAT_FOE_TARGET_OP_INIT:
        f->erased_watermark = 0U;
        break;
    case ECAT_FOE_TARGET_OP_INVALIDATE:
        f->valid = false;
        break;
    case ECAT_FOE_TARGET_OP_ERASE_TO: {
        uint32_t end = (op->need_end + 255U) & ~255U;
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
        break;
    case ECAT_FOE_TARGET_OP_FINALIZE: {
        uint16_t exp_tid = f->expected_target_id != 0U ? f->expected_target_id : ECAT_FOE_TARGET_MCU;
        if (f->fail_finalize || op->target_id != exp_tid || op->fw_version != 0x01000000U)
            return ECAT_FOE_TARGET_ASYNC_FAILED;
        f->valid = true;
        break;
    }
    }
    f->pending = false;
    return ECAT_FOE_TARGET_ASYNC_COMPLETE;
}

static void flash_abort(void *ctx)
{
    async_flash_t *f = (async_flash_t *)ctx;
    f->pending = false;
    f->resource_acquired = false;
}

static void flash_abort_session(void *ctx)
{
    async_flash_t *f = (async_flash_t *)ctx;
    f->pending = false;
    f->resource_acquired = false;
    if (!f->safe_session_abort) {
        /* 模拟旧版驱动在 abort_session 中无条件将 valid 置 false */
        f->valid = false;
    }
    f->aborts++;
}

static const ecat_foe_target_async_ops_t flash_ops = {
    flash_start, flash_poll, flash_abort, flash_abort_session
};

static unsigned floor_calls;
int port_get_version_floor(uint16_t target_id, uint32_t *floor_ver)
{
    (void)target_id;
    floor_calls++;
    *floor_ver = 0U;
    return 0;
}

int port_set_version_floor(uint16_t target_id, uint32_t new_floor_ver)
{
    (void)target_id;
    (void)new_floor_ver;
    return 0;
}

/* An asynchronous firmware target needs only its firmware async driver;
 * requiring unrelated synchronous callbacks defeats the unified path. */
static int test_async_firmware_without_sync_driver(void)
{
    async_flash_t flash = {0};
    ecat_foe_core_t core;
    ecat_foe_core_cfg_t cfg = {0};
    ecat_foe_core_async_t async;
    ecat_target_registry_t registry;
    ecat_transfer_t transfer;
    uint8_t packet[128], operation[128], staging[128];
    uint8_t image[ECAT_FOE_EFW_HEADER_MIN_LEN + 16U];
    const uint8_t payload[16] = {1U, 2U, 3U};
    ecat_target_desc_t target = {0};
    ecat_transfer_request_t request = {1U, 1U, ECAT_TRANSFER_DIR_IMPORT, 0U, 0x01000000U};
    uint64_t token = 0;
    int owner = 0;
    target.target_id = 1U;
    memcpy(target.name, "mcu", sizeof("mcu"));
    target.type = ECAT_TARGET_TYPE_FIRMWARE;
    target.permissions = ECAT_PERM_WRITE;
    target.max_size = sizeof(flash.flash);
    target.write_unit = 4U;
    target.enabled = true;
    target.asynchronous = true;
    target.driver.fw_async_ops = &flash_ops;
    target.driver.fw_async_user_ctx = &flash;
    CHECK(ecat_target_registry_init(&registry) == 0);
    CHECK(ecat_target_registry_add(&registry, &target) == 0);

    cfg.project_id = 0x10010001U;
    cfg.project_name = "SERVO_DRV";
    cfg.hardware_id = 1U;
    cfg.local_hw_ver = 1U;
    cfg.max_header_size = ECAT_FOE_EFW_HEADER_MIN_LEN;
    cfg.registry = &registry;
    CHECK(ecat_foe_core_init(&core, &cfg) == 0);
    CHECK(ecat_foe_core_async_init(&async, &core, packet, sizeof(packet), operation, sizeof(operation)) == 0);
    CHECK(ecat_transfer_init(&transfer, &registry, &core) == 0);
    CHECK(ecat_transfer_init_async_firmware(&transfer, &async) == 0);
    CHECK(ecat_transfer_set_staging_buffer(&transfer, staging, sizeof(staging)) == 0);
    CHECK(ecat_transfer_begin(&transfer, &owner, &request, &token) == ECAT_TRANSFER_COMPLETE);
    CHECK(ecat_transfer_init(&transfer, &registry, &core) == -1);
    CHECK(ecat_upgrade_session_owns_mgr(transfer.session_ptr, &transfer, &owner, token));
    ecat_foe_core_async_t replacement = {0};
    uint8_t other_packet[128], other_operation[128];
    CHECK(ecat_foe_core_async_init(&replacement, &core, other_packet, sizeof(other_packet),
                                  other_operation, sizeof(other_operation)) == -1);
    CHECK(core.async_abort_ctx == &async);
    CHECK(ecat_transfer_init_async_firmware(&transfer, &async) == -1);
    size_t image_len = make_efw(image, payload, sizeof(payload));
    for (size_t pos = 0; pos < image_len;) {
        size_t length = image_len - pos > sizeof(staging) ? sizeof(staging) : image_len - pos;
        ecat_transfer_result_t result = ecat_transfer_write(&transfer, &owner, token, image + pos, length);
        for (unsigned poll = 0; result == ECAT_TRANSFER_PENDING && poll < 64U; ++poll)
            result = ecat_transfer_poll(&transfer, &owner, token);
        CHECK(result == ECAT_TRANSFER_COMPLETE);
        pos += length;
    }
    ecat_transfer_result_t result = ecat_transfer_finish(&transfer, &owner, token);
    for (unsigned poll = 0; result == ECAT_TRANSFER_PENDING && poll < 64U; ++poll)
        result = ecat_transfer_poll(&transfer, &owner, token);
    CHECK(result == ECAT_TRANSFER_COMPLETE);
    CHECK(flash.valid && memcmp(flash.flash, payload, sizeof(payload)) == 0);
    CHECK(ecat_transfer_cancel(&transfer, &owner, token) == ECAT_TRANSFER_STALE);
    /* Replacing the idle core session invalidates managers bound to the old
     * session. They must fail before touching hardware, then may rebind. */
    CHECK(ecat_foe_core_async_init(&replacement, &core, other_packet, sizeof(other_packet),
                                  other_operation, sizeof(other_operation)) == 0);
    CHECK(ecat_transfer_begin(&transfer, &owner, &request, &token) == ECAT_TRANSFER_FAILED);
    CHECK(!transfer.active && transfer.session.owner == NULL && !flash.pending);
    CHECK(ecat_transfer_init_async_firmware(&transfer, &replacement) == 0);
    CHECK(ecat_transfer_begin(&transfer, &owner, &request, &token) == ECAT_TRANSFER_COMPLETE);
    CHECK(ecat_transfer_write(&transfer, &owner, token, image, 128U) == ECAT_TRANSFER_COMPLETE);
    CHECK(ecat_transfer_write(&transfer, &owner, token, image + 128U, image_len - 128U) == ECAT_TRANSFER_PENDING);
    CHECK(flash.pending && flash.resource_acquired);
    CHECK(ecat_transfer_cancel(&transfer, &owner, token) == ECAT_TRANSFER_COMPLETE);
    CHECK(!flash.pending && !flash.resource_acquired && !replacement.runner.pending);
    registry.targets[0].asynchronous = false;
    CHECK(ecat_transfer_begin(&transfer, &owner, &request, &token) == ECAT_TRANSFER_FAILED);
    CHECK(!transfer.active && transfer.session.owner == NULL);
    registry.targets[0].asynchronous = true;
    registry.targets[0].driver.fw_async_ops = NULL;
    CHECK(ecat_transfer_begin(&transfer, &owner, &request, &token) == ECAT_TRANSFER_FAILED);
    CHECK(!transfer.active && transfer.session.owner == NULL);
    registry.targets[0].driver.fw_async_ops = &flash_ops;
    CHECK(ecat_foe_core_deinit(&core) == 0);
    return 0;
}

static int setup_async_transfer(ecat_transfer_t *transfer,
                                ecat_foe_core_t *core, ecat_foe_core_async_t *async,
                                uint8_t *staging, size_t capacity)
{
    if (core == NULL || core->registry == NULL) return -1;
    if (ecat_transfer_init(transfer, (ecat_target_registry_t *)core->registry, core) != 0 ||
        ecat_transfer_init_async_firmware(transfer, async) != 0 ||
        ecat_transfer_set_staging_buffer(transfer, staging, capacity) != 0) return -1;
    return 0;
}

static ecat_target_registry_t ssc_registry;
static ecat_transfer_t ssc_transfer;
static uint8_t ssc_transfer_staging[128];

static int setup_managed_ssc_async(ecat_foe_core_t *core, ecat_foe_core_async_t *session,
    ecat_foe_ssc_adapter_t *adapter, ecat_foe_ssc_async_t *gate,
    uint8_t *packet, size_t packet_size)
{
    if (setup_async_transfer(&ssc_transfer, core, session,
                             ssc_transfer_staging, sizeof(ssc_transfer_staging)) != 0) return -1;
    ecat_foe_ssc_init_async(adapter, core, gate);
    if (ecat_foe_ssc_async_init(gate, ecat_foe_transport_async_ops(), &adapter->transport,
            packet, packet_size, 100U, 0x800AU, ECAT_FOE_SSC_ERR_PACKETNO,
            ECAT_FOE_SSC_ERR_PROGERROR) != 0) return -1;
    return ecat_foe_ssc_bind_transfer(adapter, &ssc_transfer);
}

int main(void)
{
    CHECK(test_async_firmware_without_sync_driver() == 0);
    static async_flash_t flash;
    static uint8_t packet_storage[128];
    static uint8_t operation_storage[128];
    static uint8_t async_packet[128];
    static ecat_foe_core_t core;
    static ecat_foe_core_async_t session;
    static ecat_foe_ssc_async_t gate;
    static ecat_foe_ssc_adapter_t adapter;
    static uint8_t efw[ECAT_FOE_EFW_HEADER_MIN_LEN + 130U];
    uint8_t payload[130U];
    ecat_foe_core_cfg_t cfg;

    memset(&flash, 0, sizeof(flash));
    {
        ecat_foe_target_async_op_t op = {0};
        op.kind = ECAT_FOE_TARGET_OP_ERASE_TO;
        op.need_end = 1U;
        CHECK(flash_poll(&flash, &op) == ECAT_FOE_TARGET_ASYNC_COMPLETE);
        flash.flash[0] = 0x12U;
        op.need_end = 257U;
        CHECK(flash_poll(&flash, &op) == ECAT_FOE_TARGET_ASYNC_COMPLETE);
        CHECK(flash.flash[0] == 0x12U);
    }
    memset(&flash, 0, sizeof(flash));
    memset(flash.flash, 0xFF, sizeof(flash.flash));
    for (size_t i = 0U; i < sizeof(payload); i++) payload[i] = (uint8_t)(i ^ 0x5AU);
    CHECK(ecat_target_registry_init(&ssc_registry) == 0);
    {
        ecat_target_desc_t t_mcu = {0};
        t_mcu.target_id = ECAT_FOE_TARGET_MCU;
        memcpy(t_mcu.name, "mcu", sizeof("mcu"));
        t_mcu.type = ECAT_TARGET_TYPE_FIRMWARE;
        t_mcu.permissions = ECAT_PERM_WRITE;
        t_mcu.max_size = sizeof(flash.flash);
        t_mcu.write_unit = 4U;
        t_mcu.enabled = true;
        t_mcu.asynchronous = true;
        t_mcu.driver.fw_async_ops = &flash_ops;
        t_mcu.driver.fw_async_user_ctx = &flash;
        CHECK(ecat_target_registry_add(&ssc_registry, &t_mcu) == 0);
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.project_id = 0x10010001U;
    cfg.project_name = "SERVO_DRV";
    cfg.hardware_id = 1U;
    cfg.local_hw_ver = 1U;
    cfg.max_header_size = ECAT_FOE_EFW_HEADER_MIN_LEN;
    cfg.registry = &ssc_registry;
    CHECK(ecat_foe_core_init(&core, &cfg) == 0);
    CHECK(ecat_foe_core_async_init(&session, &core, async_packet,
          sizeof(async_packet), operation_storage, sizeof(operation_storage)) == 0);
    CHECK(setup_managed_ssc_async(&core, &session, &adapter, &gate,
              packet_storage, sizeof(packet_storage)) == 0);

    const char name[] = "SERVO_DRV_MCU_V1.0.0.0.efw";
    CHECK(ecat_foe_ssc_on_write(&adapter, name, (uint16_t)(sizeof(name) - 1U), 0U) == 0U);
    size_t total = make_efw(efw, payload, sizeof(payload));

    /* First DATA ends inside the EFW header and is ACKed without touching Flash. */
    memcpy(packet_storage, efw, 40U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, packet_storage, 40U, true, 1U) == 0U);
    CHECK(flash.starts == 0U && core.header_rx_len == 40U);

    /* Another fragment leaves the header split across SSC packets. */
    size_t second_len = 100U;
    memcpy(packet_storage, efw + 40U, second_len);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, packet_storage,
          (uint16_t)second_len, true, 2U) == 0U);
    CHECK(flash.starts == 0U && core.header_rx_len == 140U);

    /* Third packet completes the cross-packet header and writes 120 payload
     * bytes asynchronously. Retrying that packet cannot enqueue duplicate IO. */
    size_t third_len = 128U;
    memcpy(packet_storage, efw + 140U, third_len);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, packet_storage,
          (uint16_t)third_len, true, 3U) == 0x800AU);
    unsigned starts_after_first = flash.starts;
    CHECK(ecat_foe_ssc_on_data_following(&adapter, packet_storage,
          (uint16_t)third_len, true, 4U) == 0x800AU);
    CHECK(flash.starts == starts_after_first);
    for (unsigned i = 0U; i < 32U && gate.pending; i++) {
        (void)ecat_foe_ssc_on_main_loop(&adapter, 5U + i);
    }
    CHECK(gate.completed);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, packet_storage,
          (uint16_t)third_len, true, 40U) == 0U);

    size_t final_offset = 140U + third_len;
    size_t final_len = total - final_offset;
    memcpy(packet_storage, efw + final_offset, final_len);
    uint16_t result = ecat_foe_ssc_on_data_following(&adapter, packet_storage,
          (uint16_t)final_len, false, 41U);
    CHECK(result == 0x800AU);
    for (unsigned i = 0U; i < 64U && gate.pending; i++) {
        (void)ecat_foe_ssc_on_main_loop(&adapter, 42U + i);
    }
    CHECK(gate.completed && core.state == ECAT_FOE_STATE_SUCCESS);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, packet_storage,
          (uint16_t)final_len, false, 110U) == 0U);
    CHECK(flash.valid && flash.aborts == 0U);
    CHECK(memcmp(flash.flash, payload, sizeof(payload)) == 0);
    CHECK(flash.starts >= 10U && flash.polls == flash.starts);

    /* An acknowledged partial session is still owned by the async target.
     * SSC abort must release it even when the gate has no pending DATA. */
    CHECK(ecat_foe_ssc_on_write(&adapter, name, (uint16_t)(sizeof(name) - 1U), 0U) == 0U);
    memcpy(packet_storage, efw, 128U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, packet_storage, 128U, true, 120U) == 0U);
    memcpy(packet_storage, efw + 128U, 20U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, packet_storage, 20U, true, 121U) == 0x800AU);
    for (unsigned i = 0U; i < 8U && gate.pending; i++) {
        (void)ecat_foe_ssc_on_main_loop(&adapter, 122U + i);
    }
    CHECK(gate.completed);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, packet_storage, 20U, true, 131U) == 0U);
    ecat_foe_ssc_on_abort(&adapter, ECAT_FOE_SSC_ERR_PACKETNO);
    CHECK(flash.aborts == 1U && !flash.valid && core.state == ECAT_FOE_STATE_IDLE);

    /* A header-crossing packet with too much payload must not touch the old image. */
    flash.valid = true;
    unsigned before = flash.starts;
    size_t short_total = make_efw(efw, payload, 6U);
    efw[short_total] = 0xEEU;
    CHECK(ecat_foe_ssc_on_write(&adapter, name, (uint16_t)(sizeof(name) - 1U), 0U) == 0U);
    memcpy(packet_storage, efw, 128U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, packet_storage, 128U, true, 140U) == 0U);
    memcpy(packet_storage, efw + 128U, short_total + 1U - 128U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, packet_storage,
          (uint16_t)(short_total + 1U - 128U), false, 141U) == ECAT_FOE_SSC_ERR_PROGERROR);
    CHECK(flash.valid && flash.starts == before);

    /* An immediately completed normal write should ACK without a poll/retry. */
    flash.immediate = true;
    (void)make_efw(efw, payload, sizeof(payload));
    CHECK(ecat_foe_ssc_on_write(&adapter, name, (uint16_t)(sizeof(name) - 1U), 0U) == 0U);
    memcpy(packet_storage, efw, 128U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, packet_storage, 128U, true, 150U) == 0U);
    memcpy(packet_storage, efw + 128U, 100U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, packet_storage, 100U, true, 151U) == 0U);
    CHECK(!gate.pending && core.payload_rx_bytes == 80U && core.written_offset == 80U);

    /* A valid header with an undersized operation buffer cannot invalidate. */
    ecat_foe_ssc_on_abort(&adapter, ECAT_FOE_SSC_ERR_PACKETNO);
    CHECK(ecat_foe_core_deinit(&core) == 0);
    ssc_registry.targets[0].write_unit = 256U;
    CHECK(ecat_foe_core_init(&core, &cfg) == 0);
    CHECK(ecat_foe_core_async_init(&session, &core, async_packet,
          sizeof(async_packet), operation_storage, sizeof(operation_storage)) == 0);
    CHECK(setup_managed_ssc_async(&core, &session, &adapter, &gate,
              packet_storage, sizeof(packet_storage)) == 0);
    flash.valid = true;
    before = flash.starts;
    CHECK(ecat_foe_ssc_on_write(&adapter, name, (uint16_t)(sizeof(name) - 1U), 0U) == 0U);
    memcpy(packet_storage, efw, 128U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, packet_storage, 128U, true, 160U) == 0U);
    memcpy(packet_storage, efw + 128U, 20U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, packet_storage, 20U, true, 161U) == ECAT_FOE_SSC_ERR_PROGERROR);
    CHECK(flash.valid && flash.starts == before);

    /* A disabled or mismatched header target must never reach the floor port. */
    CHECK(ecat_foe_core_deinit(&core) == 0);
    ssc_registry.targets[0].write_unit = 4U;
    cfg.anti_rollback_enable = true;
    CHECK(ecat_foe_core_init(&core, &cfg) == 0);
    CHECK(ecat_foe_core_async_init(&session, &core, async_packet,
          sizeof(async_packet), operation_storage, sizeof(operation_storage)) == 0);
    CHECK(setup_managed_ssc_async(&core, &session, &adapter, &gate,
              packet_storage, sizeof(packet_storage)) == 0);
    (void)make_efw(efw, payload, sizeof(payload));
    put16(efw + 20, ECAT_FOE_TARGET_FPGA);
    put32(efw + 144, ecat_foe_crc32_calc(efw, 144U));
    floor_calls = 0U;
    CHECK(ecat_foe_ssc_on_write(&adapter, name, (uint16_t)(sizeof(name) - 1U), 0U) == 0U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, efw, 128U, true, 170U) == 0U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, efw + 128U, 20U, true, 171U) == ECAT_FOE_SSC_ERR_PROGERROR);
    CHECK(floor_calls == 0U);
    cfg.anti_rollback_enable = false;
    (void)make_efw(efw, payload, sizeof(payload));

    /* A bound async session must not suppress a synchronous target's abort. */
    CHECK(ecat_foe_core_deinit(&core) == 0);
    static mock_driver_t sync_flash;
    mock_driver_init(&sync_flash);
    ssc_registry.targets[0].asynchronous = false;
    ssc_registry.targets[0].driver.io = sync_flash.driver;
    ssc_registry.targets[0].user_ctx = &sync_flash;
    ssc_registry.targets[0].write_unit = 4U;
    {
        ecat_target_desc_t t_fpga = {0};
        t_fpga.target_id = ECAT_FOE_TARGET_FPGA;
        memcpy(t_fpga.name, "fpga", sizeof("fpga"));
        t_fpga.type = ECAT_TARGET_TYPE_FIRMWARE;
        t_fpga.permissions = ECAT_PERM_WRITE;
        t_fpga.max_size = sizeof(flash.flash);
        t_fpga.write_unit = 4U;
        t_fpga.enabled = true;
        t_fpga.asynchronous = true;
        t_fpga.driver.fw_async_ops = &flash_ops;
        t_fpga.driver.fw_async_user_ctx = &flash;
        CHECK(ecat_target_registry_add(&ssc_registry, &t_fpga) == 0);
    }
    CHECK(ecat_foe_core_init(&core, &cfg) == 0);
    CHECK(ecat_foe_core_async_init(&session, &core, async_packet,
          sizeof(async_packet), operation_storage, sizeof(operation_storage)) == 0);
    CHECK(test_core_begin_filename(&core, name, sizeof(name) - 1U) == 0);
    CHECK(ecat_foe_core_on_data(&core, efw, ECAT_FOE_EFW_HEADER_MIN_LEN) == 0);
    ecat_foe_core_on_abort(&core);
    CHECK(sync_flash.count_abort == 1U);
    CHECK(test_core_begin_filename(&core, name, sizeof(name) - 1U) == 0);
    CHECK(ecat_foe_core_on_data(&core, efw, ECAT_FOE_EFW_HEADER_MIN_LEN) == 0);
    CHECK(test_core_begin_filename(&core, name, sizeof(name) - 1U) == 0);
    CHECK(sync_flash.count_abort == 2U);
    CHECK(ecat_foe_core_on_data(&core, efw, ECAT_FOE_EFW_HEADER_MIN_LEN) == 0);
    CHECK(ecat_foe_core_deinit(&core) == 0);
    CHECK(sync_flash.count_abort == 3U);

    /* Generic ownership and explicit finish, with no SSC gate involved. */
    memset(&flash, 0, sizeof(flash));
    ssc_registry.targets[0].asynchronous = true;
    ssc_registry.targets[0].driver.fw_async_ops = &flash_ops;
    ssc_registry.targets[0].driver.fw_async_user_ctx = &flash;
    CHECK(ecat_foe_core_init(&core, &cfg) == 0);
    CHECK(ecat_foe_core_async_init(&session, &core, async_packet,
          sizeof(async_packet), operation_storage, sizeof(operation_storage)) == 0);
    ecat_transfer_t transfer;
    uint8_t transfer_staging[128];
    int source = 0, contender = 0;
    uint64_t token = 0, rejected = 0;
    ecat_transfer_request_t request = {1U, ECAT_FOE_TARGET_MCU, ECAT_TRANSFER_DIR_IMPORT, 0U, 0x01000000U};
    ecat_transfer_request_t file_request = {2U, ECAT_FOE_TARGET_MCU, ECAT_TRANSFER_DIR_IMPORT, 0U, 0x01000000U};
    CHECK(setup_async_transfer(&transfer, &core, &session, transfer_staging, sizeof(transfer_staging)) == 0);
    CHECK(ecat_transfer_begin(&transfer, &source, &request, &token) == ECAT_TRANSFER_COMPLETE);
    total = make_efw(efw, payload, sizeof(payload));
    for (size_t pos = 0; pos < total;) {
        size_t length = total - pos > 128U ? 128U : total - pos;
        memcpy(packet_storage, efw + pos, length);
        ecat_transfer_result_t r = ecat_transfer_write(&transfer, &source, token, packet_storage, length);
        memset(packet_storage, 0xEE, sizeof(packet_storage));
        if (r == ECAT_TRANSFER_PENDING) {
            unsigned previous_starts = flash.starts;
            CHECK(ecat_transfer_write(&transfer, &source, token, efw, 1U) == ECAT_TRANSFER_BUSY);
            CHECK(ecat_transfer_finish(&transfer, &source, token) == ECAT_TRANSFER_BUSY);
            CHECK(ecat_transfer_poll(&transfer, &contender, token) == ECAT_TRANSFER_STALE);
            CHECK(flash.starts == previous_starts);
        }
        for (unsigned poll = 0; r == ECAT_TRANSFER_PENDING && poll < 64U; poll++)
            r = ecat_transfer_poll(&transfer, &source, token);
        CHECK(r == ECAT_TRANSFER_COMPLETE);
        pos += length;
    }
    CHECK(!flash.valid);
    CHECK(memcmp(flash.flash, payload, core.written_offset) == 0);
    CHECK(ecat_transfer_begin(&transfer, &contender, &request, &rejected) == ECAT_TRANSFER_BUSY);
    ecat_transfer_result_t end = ecat_transfer_finish(&transfer, &source, token);
    CHECK(end == ECAT_TRANSFER_PENDING);
    for (unsigned poll = 0; end == ECAT_TRANSFER_PENDING && poll < 64U; poll++)
        end = ecat_transfer_poll(&transfer, &source, token);
    CHECK(end == ECAT_TRANSFER_COMPLETE && flash.valid);
    CHECK(memcmp(flash.flash, payload, sizeof(payload)) == 0);
    CHECK(ecat_transfer_poll(&transfer, &source, token) == ECAT_TRANSFER_STALE);
    CHECK(ecat_transfer_begin(&transfer, &contender, &request, &rejected) == ECAT_TRANSFER_COMPLETE);
    CHECK(ecat_transfer_write(&transfer, &contender, rejected, efw, 128U) == ECAT_TRANSFER_COMPLETE);
    CHECK(ecat_transfer_write(&transfer, &contender, rejected, efw + 128U, 20U) == ECAT_TRANSFER_PENDING);
    CHECK(ecat_transfer_cancel(&transfer, &source, rejected) == ECAT_TRANSFER_STALE);
    CHECK(ecat_transfer_cancel(&transfer, &contender, rejected) == ECAT_TRANSFER_COMPLETE);
    CHECK(flash.aborts == 1U && !flash.pending);
    CHECK(ecat_transfer_cancel(&transfer, &contender, rejected) == ECAT_TRANSFER_STALE);

    /* SSC gate uses the shared manager, including final retry after ownership
     * has passed to another transport following a successful commit. */
    ecat_foe_ssc_init_async(&adapter, &core, &gate);
    CHECK(ecat_foe_ssc_async_init(&gate, ecat_foe_transport_async_ops(), &adapter.transport,
          packet_storage, sizeof(packet_storage), 100U, 0x800AU,
          ECAT_FOE_SSC_ERR_PACKETNO, ECAT_FOE_SSC_ERR_PROGERROR) == 0);
    CHECK(ecat_foe_ssc_bind_transfer(&adapter, &transfer) == 0);
    CHECK(ecat_transfer_begin(&transfer, &source, &request, &token) == ECAT_TRANSFER_COMPLETE);
    CHECK(ecat_foe_ssc_on_write(&adapter, name, sizeof(name) - 1U, 0U) != 0U);
    ecat_foe_ssc_on_abort(&adapter, 0U);
    CHECK(ecat_upgrade_session_owns_mgr(&transfer.session, &transfer, &source, token));
    CHECK(ecat_transfer_cancel(&transfer, &source, token) == ECAT_TRANSFER_COMPLETE);
    CHECK(ecat_foe_ssc_on_write(&adapter, name, sizeof(name) - 1U, 0U) == 0U);
    uint32_t tick = 1000U;
    for (size_t pos = 0; pos < total;) {
        size_t length = total - pos > 128U ? 128U : total - pos;
        bool more = pos + length < total;
        uint16_t r = ecat_foe_ssc_on_data_following(&adapter, efw + pos, (uint16_t)length, more, tick++);
        if (r == gate.busy_foe_code) {
            unsigned previous_starts = flash.starts;
            CHECK(ecat_foe_ssc_on_data_following(&adapter, efw + pos, (uint16_t)length, more, tick++) == gate.busy_foe_code);
            CHECK(flash.starts == previous_starts);
            for (unsigned poll = 0; gate.pending && poll < 64U; poll++)
                (void)ecat_foe_ssc_on_main_loop(&adapter, tick++);
            CHECK(gate.completed && !gate.operation_failed);
            if (!more) {
                CHECK(ecat_transfer_begin(&transfer, &source, &request, &token) == ECAT_TRANSFER_COMPLETE);
            }
            r = ecat_foe_ssc_on_data_following(&adapter, efw + pos, (uint16_t)length, more, tick++);
        }
        CHECK(r == 0U);
        pos += length;
    }
    CHECK(ecat_upgrade_session_owns_mgr(&transfer.session, &transfer, &source, token));
    ecat_foe_ssc_on_abort(&adapter, 0U);
    CHECK(ecat_upgrade_session_owns_mgr(&transfer.session, &transfer, &source, token));
    CHECK(ecat_transfer_cancel(&transfer, &source, token) == ECAT_TRANSFER_COMPLETE);

    CHECK(ecat_foe_ssc_on_write(&adapter, name, sizeof(name) - 1U, 0U) == 0U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, efw, 128U, true, tick++) == 0U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, efw + 128U, 20U, true, tick++) == gate.busy_foe_code);
    tick += 100U;
    CHECK(ecat_foe_ssc_on_main_loop(&adapter, tick++) == gate.operation_error_foe_code);
    CHECK(ecat_transfer_begin(&transfer, &source, &request, &token) == ECAT_TRANSFER_COMPLETE);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, efw + 128U, 20U, true, tick++) == gate.operation_error_foe_code);
    CHECK(ecat_upgrade_session_owns_mgr(&transfer.session, &transfer, &source, token));
    CHECK(ecat_transfer_cancel(&transfer, &source, token) == ECAT_TRANSFER_COMPLETE);

    /* Optional FILE reference also drives the same asynchronous backend. */
    FILE *stream = tmpfile();
    CHECK(stream != NULL);
    CHECK(fwrite(efw, 1U, total, stream) == total);
    rewind(stream);
    ecat_transfer_file_t file;
    uint8_t file_buffer[64];
    CHECK(ecat_transfer_file_init(&file, &transfer, file_buffer, sizeof(file_buffer)) == 0);
    CHECK(ecat_transfer_file_begin(&file, stream, &file_request) == ECAT_TRANSFER_COMPLETE);
    ecat_transfer_result_t file_result = ECAT_TRANSFER_PENDING;
    for (unsigned step = 0; file_result == ECAT_TRANSFER_PENDING && step < 256U; step++) {
        bool was_pending = file.pending;
        long position = ftell(stream);
        file_result = ecat_transfer_file_step(&file);
        if (was_pending) CHECK(ftell(stream) == position);
    }
    CHECK(file_result == ECAT_TRANSFER_COMPLETE && flash.valid);
    CHECK(memcmp(flash.flash, payload, sizeof(payload)) == 0);
    rewind(stream);
    CHECK(ecat_transfer_file_begin(&file, stream, &file_request) == ECAT_TRANSFER_COMPLETE);
    for (unsigned step = 0; !file.pending && step < 16U; step++)
        CHECK(ecat_transfer_file_step(&file) == ECAT_TRANSFER_PENDING);
    CHECK(file.pending);
    unsigned aborts_before_cancel = flash.aborts;
    CHECK(ecat_transfer_file_cancel(&file) == ECAT_TRANSFER_COMPLETE);
    CHECK(flash.aborts == aborts_before_cancel + 1U);
    CHECK(ecat_transfer_file_step(&file) == ECAT_TRANSFER_FAILED);
    fclose(stream);

    /* ---------------------------------------------------------------------- */
    /* 验证 packet_storage 与 operation_storage 部分重叠严格安全拒绝与边界相接 */
    /* ---------------------------------------------------------------------- */
    {
        uint8_t big_payload[256];
        for (size_t i = 0; i < sizeof(big_payload); i++) big_payload[i] = (uint8_t)(i ^ 0x5A);
        uint8_t big_efw[ECAT_FOE_EFW_HEADER_MIN_LEN + sizeof(big_payload)];
        size_t big_total = make_efw(big_efw, big_payload, sizeof(big_payload));
        (void)big_total;

        async_flash_t big_flash;
        memset(&big_flash, 0, sizeof(big_flash));
        big_flash.immediate = true;

        ecat_foe_core_t test_core;
        ecat_foe_core_cfg_t test_cfg;
        ecat_target_registry_t test_registry;
        CHECK(ecat_target_registry_init(&test_registry) == 0);
        {
            ecat_target_desc_t t_big = {0};
            t_big.target_id = ECAT_FOE_TARGET_MCU;
            memcpy(t_big.name, "mcu", sizeof("mcu"));
            t_big.type = ECAT_TARGET_TYPE_FIRMWARE;
            t_big.permissions = ECAT_PERM_WRITE;
            t_big.max_size = sizeof(big_flash.flash);
            t_big.write_unit = 64U;
            t_big.enabled = true;
            t_big.asynchronous = true;
            t_big.driver.fw_async_ops = &flash_ops;
            t_big.driver.fw_async_user_ctx = &big_flash;
            CHECK(ecat_target_registry_add(&test_registry, &t_big) == 0);
        }
        memset(&test_cfg, 0, sizeof(test_cfg));
        test_cfg.project_name = "DRIVE_SERVO";
        test_cfg.project_id = 0x10010001U;
        test_cfg.hardware_id = 1U;
        test_cfg.local_hw_ver = 1U;
        test_cfg.max_header_size = ECAT_FOE_EFW_HEADER_MIN_LEN;
        test_cfg.registry = &test_registry;
        CHECK(ecat_foe_core_init(&test_core, &test_cfg) == 0);

        uint8_t shared_arr[512];
        ecat_foe_core_async_t test_session;
        ecat_transfer_t test_upg;
        uint8_t test_staging[256];

        /* Case 1: 后向部分重叠拒绝 (packet = &shared_arr[0..255], op = &shared_arr[64..127]) */
        CHECK(ecat_foe_core_async_init(&test_session, &test_core,
                                       &shared_arr[0], 256U,
                                       &shared_arr[64], 64U) == -1);

        /* Case 2: 前向部分重叠拒绝 (packet = &shared_arr[32..287], op = &shared_arr[0..63]) */
        CHECK(ecat_foe_core_async_init(&test_session, &test_core,
                                       &shared_arr[32], 256U,
                                       &shared_arr[0], 64U) == -1);

        /* Case 3: 完全相同首地址拒绝 (packet = &shared_arr[0], op = &shared_arr[0]) */
        CHECK(ecat_foe_core_async_init(&test_session, &test_core,
                                       &shared_arr[0], 256U,
                                       &shared_arr[0], 64U) == -1);

        /* Case 4: 包含关系拒绝 (op 落在 packet 内部) */
        CHECK(ecat_foe_core_async_init(&test_session, &test_core,
                                       &shared_arr[0], 256U,
                                       &shared_arr[100], 64U) == -1);

        /* Case 5: 容量与地址加法溢出安全防御 */
        CHECK(ecat_foe_core_async_init(&test_session, &test_core,
                                       (uint8_t *)(UINTPTR_MAX - 10U), 20U,
                                       &shared_arr[0], 64U) == -1);
        CHECK(ecat_foe_core_async_init(&test_session, &test_core,
                                       &shared_arr[0], 128U,
                                       (uint8_t *)(UINTPTR_MAX - 10U), 20U) == -1);

        /* Case 6: 边界相接合法放行 (packet = &shared_arr[0..255], op = &shared_arr[256..319]) */
        test_core.shared_session = NULL;
        CHECK(ecat_foe_core_async_init(&test_session, &test_core,
                                       &shared_arr[0], 256U,
                                       &shared_arr[256], 64U) == 0);
        {
            CHECK(setup_async_transfer(&test_upg, &test_core, &test_session, test_staging, sizeof(test_staging)) == 0);
            int src = 1;
            ecat_transfer_request_t req = {1U, ECAT_FOE_TARGET_MCU, ECAT_TRANSFER_DIR_IMPORT, 0U, 0x01000000U};
            uint64_t tok = 0;
            CHECK(ecat_transfer_begin(&test_upg, &src, &req, &tok) == ECAT_TRANSFER_COMPLETE);

            /* 分块写入完整 EFW 文件 (含大于 op_cap 的 128 字节分块) */
            CHECK(ecat_transfer_write(&test_upg, &src, tok, big_efw, ECAT_FOE_EFW_HEADER_MIN_LEN) == ECAT_TRANSFER_COMPLETE);
            CHECK(ecat_transfer_write(&test_upg, &src, tok, big_efw + ECAT_FOE_EFW_HEADER_MIN_LEN, 128U) == ECAT_TRANSFER_COMPLETE);
            CHECK(ecat_transfer_write(&test_upg, &src, tok, big_efw + ECAT_FOE_EFW_HEADER_MIN_LEN + 128U, 128U) == ECAT_TRANSFER_COMPLETE);
            ecat_transfer_result_t fin = ecat_transfer_finish(&test_upg, &src, tok);
            for (unsigned poll = 0; fin == ECAT_TRANSFER_PENDING && poll < 64U; poll++)
                fin = ecat_transfer_poll(&test_upg, &src, tok);
            CHECK(fin == ECAT_TRANSFER_COMPLETE);

            CHECK(big_flash.valid);
            CHECK(memcmp(big_flash.flash, big_payload, sizeof(big_payload)) == 0);
            CHECK(ecat_transfer_cancel(&test_upg, &src, tok) == ECAT_TRANSFER_STALE);
        }

        /* Case 7: 反向边界相接合法放行 (op = &shared_arr[0..63], packet = &shared_arr[64..319]) */
        memset(&big_flash, 0, sizeof(big_flash));
        big_flash.immediate = true;
        ecat_foe_core_on_abort(&test_core);
        test_core.shared_session = NULL;
        CHECK(ecat_foe_core_async_init(&test_session, &test_core,
                                       &shared_arr[64], 256U,
                                       &shared_arr[0], 64U) == 0);
        {
            CHECK(setup_async_transfer(&test_upg, &test_core, &test_session, test_staging, sizeof(test_staging)) == 0);
            int src = 1;
            ecat_transfer_request_t req = {1U, ECAT_FOE_TARGET_MCU, ECAT_TRANSFER_DIR_IMPORT, 0U, 0x01000000U};
            uint64_t tok = 0;
            CHECK(ecat_transfer_begin(&test_upg, &src, &req, &tok) == ECAT_TRANSFER_COMPLETE);

            CHECK(ecat_transfer_write(&test_upg, &src, tok, big_efw, ECAT_FOE_EFW_HEADER_MIN_LEN) == ECAT_TRANSFER_COMPLETE);
            CHECK(ecat_transfer_write(&test_upg, &src, tok, big_efw + ECAT_FOE_EFW_HEADER_MIN_LEN, 128U) == ECAT_TRANSFER_COMPLETE);
            CHECK(ecat_transfer_write(&test_upg, &src, tok, big_efw + ECAT_FOE_EFW_HEADER_MIN_LEN + 128U, 128U) == ECAT_TRANSFER_COMPLETE);
            ecat_transfer_result_t fin = ecat_transfer_finish(&test_upg, &src, tok);
            for (unsigned poll = 0; fin == ECAT_TRANSFER_PENDING && poll < 64U; poll++)
                fin = ecat_transfer_poll(&test_upg, &src, tok);
            CHECK(fin == ECAT_TRANSFER_COMPLETE);

            CHECK(big_flash.valid);
            CHECK(memcmp(big_flash.flash, big_payload, sizeof(big_payload)) == 0);
        }
    }

    /* ---------------------------------------------------------------------- */
    /* 固件异步 INIT 异常生命周期与清理契约 (立即失败/PENDING失败/成功后失败/取消/旧镜像保护) */
    /* ---------------------------------------------------------------------- */
    {
        test_sig_verifier_t sig_v;
        memset(&sig_v, 0, sizeof(sig_v));
        ecat_foe_signature_verifier_t v_cbs = {
            test_sig_begin, test_sig_update, test_sig_finish, test_sig_abort, &sig_v
        };

        memset(&flash, 0, sizeof(flash));
        CHECK(ecat_target_registry_init(&ssc_registry) == 0);
        {
            ecat_target_desc_t t_mcu = {0};
            t_mcu.target_id = ECAT_FOE_TARGET_MCU;
            memcpy(t_mcu.name, "mcu", sizeof("mcu"));
            t_mcu.type = ECAT_TARGET_TYPE_FIRMWARE;
            t_mcu.permissions = ECAT_PERM_WRITE;
            t_mcu.max_size = sizeof(flash.flash);
            t_mcu.write_unit = 4U;
            t_mcu.enabled = true;
            t_mcu.asynchronous = true;
            t_mcu.driver.fw_async_ops = &flash_ops;
            t_mcu.driver.fw_async_user_ctx = &flash;
            CHECK(ecat_target_registry_add(&ssc_registry, &t_mcu) == 0);
        }
        memset(&cfg, 0, sizeof(cfg));
        cfg.project_id = 0x10010001U;
        cfg.project_name = "SERVO_DRV";
        cfg.hardware_id = 1U;
        cfg.local_hw_ver = 1U;
        cfg.max_header_size = ECAT_FOE_EFW_HEADER_MIN_LEN;
        cfg.signature_verify_enable = true;
        cfg.signature_verifier = v_cbs;
        cfg.registry = &ssc_registry;

        uint8_t signed_efw[ECAT_FOE_EFW_HEADER_MIN_LEN + sizeof(payload)];
        size_t signed_total = make_signed_efw(signed_efw, payload, sizeof(payload));
        (void)signed_total;

        /* ------------------------------------------------------------------ */
        /* Case A-1: 旧版驱动 (默认 async_safe_session_abort=false) INIT 立即失败 */
        /* 验证既有契约: 驱动自行回滚资源，框架不调用 abort_session，旧有效镜像100%保留 */
        /* ------------------------------------------------------------------ */
        ssc_registry.targets[0].async_safe_session_abort = false;
        CHECK(ecat_foe_core_deinit(&core) == 0);
        CHECK(ecat_foe_core_init(&core, &cfg) == 0);
        CHECK(ecat_foe_core_async_init(&session, &core, async_packet,
              sizeof(async_packet), operation_storage, sizeof(operation_storage)) == 0);
        CHECK(setup_managed_ssc_async(&core, &session, &adapter, &gate,
              packet_storage, sizeof(packet_storage)) == 0);

        flash.valid = true; /* 物理存储上原本已存在旧有效固件镜像 */
        flash.safe_session_abort = false; /* 模拟旧版驱动: abort_session 会破坏 valid */
        flash.fail_init_immediate = true;
        flash.resource_acquired = false;
        flash.aborts = 0U;
        memset(&sig_v, 0, sizeof(sig_v));

        uint16_t w_rc = ecat_foe_ssc_on_write(&adapter, name, (uint16_t)(sizeof(name) - 1U), 0U);
        uint16_t d1_rc = ecat_foe_ssc_on_data_following(&adapter, signed_efw, 40U, true, 201U);
        uint16_t d2_rc = ecat_foe_ssc_on_data_following(&adapter, signed_efw + 40U, 100U, true, 202U);
        CHECK(w_rc == 0U && d1_rc == 0U && d2_rc == 0U);

        /* 第三包凑满 148B 头部并触发 INIT -> 旧驱动在 start 内部自行回滚资源并立即返回 FAILED */
        uint16_t err = ecat_foe_ssc_on_data_following(&adapter, signed_efw + 140U, 128U, true, 203U);
        CHECK(err == ECAT_FOE_SSC_ERR_PROGERROR);
        CHECK(core.state == ECAT_FOE_STATE_ERROR);
        CHECK(core.last_error == ECAT_FOE_CORE_ERR_DRV_INIT);
        CHECK(!core.session_active); /* 租约已释放 */
        CHECK(!core.signature_active); /* 验签已关闭 */
        CHECK(!core.driver_invalidated);
        CHECK(flash.aborts == 0U); /* 核心保证: 对旧版驱动不调用 abort_session，防止清除 valid */
        CHECK(!flash.resource_acquired); /* 旧版驱动在 start() 中自行回滚 */
        CHECK(flash.valid); /* 关键核心保证: 旧有效镜像绝不受破坏！ */
        CHECK(sig_v.begin_calls == 1U && sig_v.abort_calls == 1U && sig_v.finish_calls == 0U);

        /* 外部再次调用 abort 不得产生多余 abort_session 调用 */
        ecat_foe_ssc_on_abort(&adapter, 0U);
        CHECK(flash.aborts == 0U);

        /* 在统一 upgrade 引擎直接写入时验证旧驱动 INIT 立即失败 */
        ecat_transfer_t upg_test;
        uint8_t upg_staging[128];
        CHECK(setup_async_transfer(&upg_test, &core, &session, upg_staging, sizeof(upg_staging)) == 0);
        int src_id = 99;
        uint64_t tok_val = 0;
        ecat_transfer_request_t req_val = {1U, ECAT_FOE_TARGET_MCU, ECAT_TRANSFER_DIR_IMPORT, 0U, 0x01000000U};
        CHECK(ecat_transfer_begin(&upg_test, &src_id, &req_val, &tok_val) == ECAT_TRANSFER_COMPLETE);
        flash.fail_init_immediate = true;
        flash.safe_session_abort = false;
        flash.resource_acquired = false;
        flash.aborts = 0U;
        memset(&sig_v, 0, sizeof(sig_v));
        CHECK(ecat_transfer_write(&upg_test, &src_id, tok_val, signed_efw, 40U) == ECAT_TRANSFER_COMPLETE);
        CHECK(ecat_transfer_write(&upg_test, &src_id, tok_val, signed_efw + 40U, 100U) == ECAT_TRANSFER_COMPLETE);
        CHECK(ecat_transfer_write(&upg_test, &src_id, tok_val, signed_efw + 140U, 128U) == ECAT_TRANSFER_FAILED);
        CHECK(core.last_error == ECAT_FOE_CORE_ERR_DRV_INIT);
        CHECK(core.state == ECAT_FOE_STATE_ERROR);
        CHECK(!core.session_active);
        CHECK(!core.signature_active);
        CHECK(flash.aborts == 0U);
        CHECK(!flash.resource_acquired);
        CHECK(flash.valid); /* 旧镜像依然受保护 */
        CHECK(sig_v.begin_calls == 1U && sig_v.abort_calls == 1U);
        CHECK(ecat_transfer_cancel(&upg_test, &src_id, tok_val) == ECAT_TRANSFER_STALE);
        ecat_foe_core_on_abort(&core);
        CHECK(core.state == ECAT_FOE_STATE_IDLE);

        /* ------------------------------------------------------------------ */
        /* Case A-2: 新版安全驱动 (显式声明 async_safe_session_abort=true)      */
        /* 验证框架负责调用 abort_session 清理部分获取的资源，且镜像依然受保护   */
        /* ------------------------------------------------------------------ */
        ssc_registry.targets[0].async_safe_session_abort = true;
        CHECK(ecat_foe_core_deinit(&core) == 0);
        CHECK(ecat_foe_core_init(&core, &cfg) == 0);
        CHECK(ecat_foe_core_async_init(&session, &core, async_packet,
              sizeof(async_packet), operation_storage, sizeof(operation_storage)) == 0);
        CHECK(setup_managed_ssc_async(&core, &session, &adapter, &gate,
              packet_storage, sizeof(packet_storage)) == 0);

        flash.valid = true;
        flash.safe_session_abort = true;
        flash.fail_init_immediate = true;
        flash.resource_acquired = false;
        flash.aborts = 0U;
        memset(&sig_v, 0, sizeof(sig_v));

        CHECK(ecat_foe_ssc_on_write(&adapter, name, (uint16_t)(sizeof(name) - 1U), 0U) == 0U);
        CHECK(ecat_foe_ssc_on_data_following(&adapter, signed_efw, 40U, true, 201U) == 0U);
        CHECK(ecat_foe_ssc_on_data_following(&adapter, signed_efw + 40U, 100U, true, 202U) == 0U);
        err = ecat_foe_ssc_on_data_following(&adapter, signed_efw + 140U, 128U, true, 203U);
        CHECK(err == ECAT_FOE_SSC_ERR_PROGERROR);
        CHECK(core.state == ECAT_FOE_STATE_ERROR);
        CHECK(core.last_error == ECAT_FOE_CORE_ERR_DRV_INIT);
        CHECK(!core.session_active);
        CHECK(!core.signature_active);
        CHECK(!core.driver_invalidated);
        CHECK(flash.aborts == 1U); /* 框架保证且仅保证调用一次 abort_session */
        CHECK(!flash.resource_acquired); /* 驱动半成品资源被 abort_session 回收 */
        CHECK(flash.valid); /* 旧有效镜像依然受保护 */
        CHECK(sig_v.begin_calls == 1U && sig_v.abort_calls == 1U && sig_v.finish_calls == 0U);

        /* 外部再次调用 abort 不得产生第二次 abort_session 调用 */
        ecat_foe_ssc_on_abort(&adapter, 0U);
        CHECK(flash.aborts == 1U);

        /* 在统一 upgrade 引擎直接写入时验证新版安全驱动 INIT 立即失败 */
        CHECK(setup_async_transfer(&upg_test, &core, &session, upg_staging, sizeof(upg_staging)) == 0);
        tok_val = 0;
        CHECK(ecat_transfer_begin(&upg_test, &src_id, &req_val, &tok_val) == ECAT_TRANSFER_COMPLETE);
        flash.fail_init_immediate = true;
        flash.safe_session_abort = true;
        flash.resource_acquired = false;
        flash.aborts = 0U;
        memset(&sig_v, 0, sizeof(sig_v));
        CHECK(ecat_transfer_write(&upg_test, &src_id, tok_val, signed_efw, 40U) == ECAT_TRANSFER_COMPLETE);
        CHECK(ecat_transfer_write(&upg_test, &src_id, tok_val, signed_efw + 40U, 100U) == ECAT_TRANSFER_COMPLETE);
        CHECK(ecat_transfer_write(&upg_test, &src_id, tok_val, signed_efw + 140U, 128U) == ECAT_TRANSFER_FAILED);
        CHECK(core.last_error == ECAT_FOE_CORE_ERR_DRV_INIT);
        CHECK(core.state == ECAT_FOE_STATE_ERROR);
        CHECK(!core.session_active);
        CHECK(!core.signature_active);
        CHECK(flash.aborts == 1U);
        CHECK(!flash.resource_acquired);
        CHECK(flash.valid);
        CHECK(sig_v.begin_calls == 1U && sig_v.abort_calls == 1U);
        CHECK(ecat_transfer_cancel(&upg_test, &src_id, tok_val) == ECAT_TRANSFER_STALE);
        ecat_foe_core_on_abort(&core);
        CHECK(core.state == ECAT_FOE_STATE_IDLE);

        /* 验证错误后可被新 WRQ 会话重新复用并正常升级 */
        flash.fail_init_immediate = false;
        flash.aborts = 0U;
        memset(&sig_v, 0, sizeof(sig_v));
        CHECK(ecat_foe_ssc_on_write(&adapter, name, (uint16_t)(sizeof(name) - 1U), 0U) == 0U);
        CHECK(ecat_foe_ssc_on_data_following(&adapter, signed_efw, 40U, true, 204U) == 0U);
        CHECK(ecat_foe_ssc_on_data_following(&adapter, signed_efw + 40U, 100U, true, 205U) == 0U);
        CHECK(ecat_foe_ssc_on_data_following(&adapter, signed_efw + 140U, 128U, true, 206U) == 0x800AU);
        for (unsigned i = 0U; i < 32U && gate.pending; i++) {
            (void)ecat_foe_ssc_on_main_loop(&adapter, 207U + i);
        }
        CHECK(gate.completed && !gate.operation_failed);
        ecat_foe_ssc_on_abort(&adapter, 0U);
        CHECK(flash.aborts == 1U);

        /* Case B: start(INIT) 返回 PENDING 后 poll(INIT) 失败 */
        flash.valid = true;
        flash.safe_session_abort = true;
        flash.fail_init_poll = true;
        flash.fail_init_immediate = false;
        flash.resource_acquired = false;
        flash.aborts = 0U;
        memset(&sig_v, 0, sizeof(sig_v));
        CHECK(ecat_foe_core_deinit(&core) == 0);
        CHECK(ecat_foe_core_init(&core, &cfg) == 0);
        CHECK(ecat_foe_core_async_init(&session, &core, async_packet,
              sizeof(async_packet), operation_storage, sizeof(operation_storage)) == 0);
        CHECK(setup_managed_ssc_async(&core, &session, &adapter, &gate,
              packet_storage, sizeof(packet_storage)) == 0);

        CHECK(ecat_foe_ssc_on_write(&adapter, name, (uint16_t)(sizeof(name) - 1U), 0U) == 0U);
        CHECK(ecat_foe_ssc_on_data_following(&adapter, signed_efw, 40U, true, 210U) == 0U);
        CHECK(ecat_foe_ssc_on_data_following(&adapter, signed_efw + 40U, 100U, true, 211U) == 0U);
        CHECK(ecat_foe_ssc_on_data_following(&adapter, signed_efw + 140U, 128U, true, 212U) == 0x800AU);
        CHECK(gate.pending && flash.resource_acquired);
        CHECK(ecat_foe_ssc_on_main_loop(&adapter, 213U) == ECAT_FOE_SSC_ERR_PROGERROR);
        CHECK(core.last_error == ECAT_FOE_CORE_ERR_DRV_INIT); /* 失败清理后保留诊断 */
        CHECK(!core.session_active);
        CHECK(!core.signature_active);
        CHECK(!core.driver_invalidated);
        CHECK(flash.aborts == 1U);
        CHECK(!flash.resource_acquired);
        CHECK(flash.valid); /* 旧镜像依然受保护 */
        CHECK(sig_v.begin_calls == 1U && sig_v.abort_calls == 1U);

        /* Case C: INIT 成功但后续 INVALIDATE 失败 */
        flash.safe_session_abort = true;
        flash.fail_init_poll = false;
        flash.fail_invalidate = true;
        flash.resource_acquired = false;
        flash.aborts = 0U;
        memset(&sig_v, 0, sizeof(sig_v));
        CHECK(ecat_foe_core_deinit(&core) == 0);
        CHECK(ecat_foe_core_init(&core, &cfg) == 0);
        CHECK(ecat_foe_core_async_init(&session, &core, async_packet,
              sizeof(async_packet), operation_storage, sizeof(operation_storage)) == 0);
        CHECK(setup_managed_ssc_async(&core, &session, &adapter, &gate,
              packet_storage, sizeof(packet_storage)) == 0);

        CHECK(ecat_foe_ssc_on_write(&adapter, name, (uint16_t)(sizeof(name) - 1U), 0U) == 0U);
        CHECK(ecat_foe_ssc_on_data_following(&adapter, signed_efw, 40U, true, 220U) == 0U);
        CHECK(ecat_foe_ssc_on_data_following(&adapter, signed_efw + 40U, 100U, true, 221U) == 0U);
        CHECK(ecat_foe_ssc_on_data_following(&adapter, signed_efw + 140U, 128U, true, 222U) == 0x800AU);
        /* 在 poll 中 INIT 成功，紧接着 INVALIDATE 失败 */
        for (unsigned i = 0U; i < 32U && gate.pending; i++) {
            (void)ecat_foe_ssc_on_main_loop(&adapter, 223U + i);
        }
        CHECK(gate.operation_failed);
        CHECK(!core.session_active);
        CHECK(flash.aborts == 1U);
        CHECK(!flash.resource_acquired);
        CHECK(sig_v.begin_calls == 1U && sig_v.abort_calls == 1U);

        /* Case D: 在 INIT PENDING 期间外部取消 */
        flash.safe_session_abort = true;
        flash.fail_invalidate = false;
        flash.valid = true;
        flash.resource_acquired = false;
        flash.aborts = 0U;
        memset(&sig_v, 0, sizeof(sig_v));
        CHECK(ecat_foe_core_deinit(&core) == 0);
        CHECK(ecat_foe_core_init(&core, &cfg) == 0);
        CHECK(ecat_foe_core_async_init(&session, &core, async_packet,
              sizeof(async_packet), operation_storage, sizeof(operation_storage)) == 0);
        CHECK(setup_managed_ssc_async(&core, &session, &adapter, &gate,
              packet_storage, sizeof(packet_storage)) == 0);

        CHECK(ecat_foe_ssc_on_write(&adapter, name, (uint16_t)(sizeof(name) - 1U), 0U) == 0U);
        CHECK(ecat_foe_ssc_on_data_following(&adapter, signed_efw, 40U, true, 230U) == 0U);
        CHECK(ecat_foe_ssc_on_data_following(&adapter, signed_efw + 40U, 100U, true, 231U) == 0U);
        CHECK(ecat_foe_ssc_on_data_following(&adapter, signed_efw + 140U, 128U, true, 232U) == 0x800AU);
        CHECK(gate.pending && flash.resource_acquired);
        ecat_foe_ssc_on_abort(&adapter, ECAT_FOE_SSC_ERR_PACKETNO);
        CHECK(flash.aborts == 1U);
        CHECK(!flash.resource_acquired);
        CHECK(!flash.pending);
        CHECK(flash.valid); /* 未执行过 INVALIDATE，旧镜像不受影响 */
        CHECK(!core.session_active);
        CHECK(sig_v.begin_calls == 1U && sig_v.abort_calls == 1U);

        /* Case E: runner 级 ecat_foe_target_async_abort_session 契约行为核验 */
        {
            ecat_foe_target_async_t test_runner;
            uint8_t dummy_storage[64];
            flash.safe_session_abort = true;
            flash.aborts = 0U;
            flash.pending = false;
            CHECK(ecat_foe_target_async_init(&test_runner, &flash_ops, &flash,
                                             dummy_storage, sizeof(dummy_storage)) == 0);
            /* 当处于 pending 态时调用 abort_session: 触发 ops->abort 与 ops->abort_session */
            test_runner.pending = true;
            ecat_foe_target_async_abort_session(&test_runner);
            CHECK(!test_runner.pending);
            CHECK(test_runner.failed);
            CHECK(flash.aborts == 1U);
            /* 非 pending 态下再次调用: ops->abort 不再调用，ops->abort_session 按单次调用转发 */
            ecat_foe_target_async_abort_session(&test_runner);
            CHECK(flash.aborts == 2U);
            /* 验证重置后可重新开始 */
            CHECK(ecat_foe_target_async_reset(&test_runner) == 0);
            CHECK(!test_runner.failed);
        }

        /* Case F: 高编号目标 (65535) 异步升级未完成位图隔离 + 防回滚 FINALIZE 失败错误码一致性 */
        {
            ecat_target_registry_t high_reg;
            ecat_foe_core_t high_core;
            ecat_foe_core_async_t high_sess;
            ecat_transfer_t high_xfer;
            ecat_foe_core_cfg_t high_cfg = {0};
            uint8_t high_staging[128];
            uint8_t high_efw[ECAT_FOE_EFW_HEADER_MIN_LEN + 16U];
            const uint8_t high_payload[16] = {0x99U, 0x88U, 0x77U};
            ecat_target_desc_t high_desc = {0};

            memset(&flash, 0, sizeof(flash));
            flash.expected_target_id = 65535U;
            flash.safe_session_abort = true;

            CHECK(ecat_target_registry_init(&high_reg) == 0);
            high_desc.target_id = 65535U;
            memcpy(high_desc.name, "high_fw", sizeof("high_fw"));
            high_desc.type = ECAT_TARGET_TYPE_FIRMWARE;
            high_desc.permissions = ECAT_PERM_WRITE;
            high_desc.max_size = sizeof(flash.flash);
            high_desc.write_unit = 4U;
            high_desc.enabled = true;
            high_desc.asynchronous = true;
            high_desc.async_safe_session_abort = true;
            high_desc.driver.fw_async_ops = &flash_ops;
            high_desc.driver.fw_async_user_ctx = &flash;
            CHECK(ecat_target_registry_add(&high_reg, &high_desc) == 0);

            high_cfg.project_id = 0x10010001U;
            high_cfg.project_name = "SERVO_DRV";
            high_cfg.hardware_id = 1U;
            high_cfg.local_hw_ver = 1U;
            high_cfg.anti_rollback_enable = true;
            high_cfg.max_header_size = ECAT_FOE_EFW_HEADER_MIN_LEN;
            high_cfg.registry = &high_reg;
            CHECK(ecat_foe_core_init(&high_core, &high_cfg) == 0);
            CHECK(ecat_foe_core_async_init(&high_sess, &high_core, async_packet,
                  sizeof(async_packet), operation_storage, sizeof(operation_storage)) == 0);
            CHECK(setup_async_transfer(&high_xfer, &high_core, &high_sess,
                  high_staging, sizeof(high_staging)) == 0);

            size_t h_len = make_efw(high_efw, high_payload, sizeof(high_payload));
            put16(high_efw + 20, 65535U);
            put32(high_efw + 144, ecat_foe_crc32_calc(high_efw, 144U));

            int owner_id = 77;
            uint64_t h_tok = 0;
            ecat_transfer_request_t h_req = {1U, 65535U, ECAT_TRANSFER_DIR_IMPORT, 0U, 0x01000000U};

            /* 1) PENDING 模式下防回滚 FINALIZE 失败必须返回 ECAT_FOE_CORE_ERR_ANTI_ROLLBACK_UPDATE */
            flash.immediate = false;
            flash.fail_finalize = true;
            CHECK(ecat_transfer_begin(&high_xfer, &owner_id, &h_req, &h_tok) == ECAT_TRANSFER_COMPLETE);
            for (size_t pos = 0; pos < h_len;) {
                size_t chunk = (h_len - pos > 128U) ? 128U : (h_len - pos);
                ecat_transfer_result_t wr = ecat_transfer_write(&high_xfer, &owner_id, h_tok, high_efw + pos, chunk);
                for (unsigned p = 0; wr == ECAT_TRANSFER_PENDING && p < 64U; p++)
                    wr = ecat_transfer_poll(&high_xfer, &owner_id, h_tok);
                CHECK(wr == ECAT_TRANSFER_COMPLETE);
                pos += chunk;
            }
            CHECK(ecat_foe_core_upgrade_incomplete(&high_core));
            CHECK(ecat_foe_core_target_upgrade_incomplete(&high_core, 65535U));
            CHECK(!ecat_foe_core_target_upgrade_incomplete(&high_core, 1U));

            ecat_transfer_result_t fr = ecat_transfer_finish(&high_xfer, &owner_id, h_tok);
            for (unsigned p = 0; fr == ECAT_TRANSFER_PENDING && p < 64U; p++)
                fr = ecat_transfer_poll(&high_xfer, &owner_id, h_tok);
            CHECK(fr == ECAT_TRANSFER_FAILED);
            CHECK(high_core.last_error == ECAT_FOE_CORE_ERR_ANTI_ROLLBACK_UPDATE);
            CHECK(ecat_foe_core_target_upgrade_incomplete(&high_core, 65535U));

            /* 2) 立即模式下防回滚 FINALIZE 失败同样返回 ECAT_FOE_CORE_ERR_ANTI_ROLLBACK_UPDATE */
            flash.immediate = true;
            flash.fail_finalize = true;
            CHECK(ecat_transfer_begin(&high_xfer, &owner_id, &h_req, &h_tok) == ECAT_TRANSFER_COMPLETE);
            for (size_t pos = 0; pos < h_len;) {
                size_t chunk = (h_len - pos > 128U) ? 128U : (h_len - pos);
                CHECK(ecat_transfer_write(&high_xfer, &owner_id, h_tok, high_efw + pos, chunk) == ECAT_TRANSFER_COMPLETE);
                pos += chunk;
            }
            CHECK(ecat_transfer_finish(&high_xfer, &owner_id, h_tok) == ECAT_TRANSFER_FAILED);
            CHECK(high_core.last_error == ECAT_FOE_CORE_ERR_ANTI_ROLLBACK_UPDATE);

            /* 3) 恢复正常后，高编号目标 65535 提交成功并清除未闭环槽位状态 */
            flash.fail_finalize = false;
            CHECK(ecat_transfer_begin(&high_xfer, &owner_id, &h_req, &h_tok) == ECAT_TRANSFER_COMPLETE);
            for (size_t pos = 0; pos < h_len;) {
                size_t chunk = (h_len - pos > 128U) ? 128U : (h_len - pos);
                CHECK(ecat_transfer_write(&high_xfer, &owner_id, h_tok, high_efw + pos, chunk) == ECAT_TRANSFER_COMPLETE);
                pos += chunk;
            }
            CHECK(ecat_transfer_finish(&high_xfer, &owner_id, h_tok) == ECAT_TRANSFER_COMPLETE);
            CHECK(flash.valid && memcmp(flash.flash, high_payload, sizeof(high_payload)) == 0);
            CHECK(!ecat_foe_core_upgrade_incomplete(&high_core));
            CHECK(!ecat_foe_core_target_upgrade_incomplete(&high_core, 65535U));
            CHECK(ecat_foe_core_deinit(&high_core) == 0);
        }
    }

    puts("PASS: async core header split, SSC BUSY/retry, Flash write/readback and finalize");
    return 0;
}
