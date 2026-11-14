#include "ecat_foe_core_async.h"
#include "ecat_foe_ssc.h"
#include "mock_driver.h"
#include "ecat_upgrade.h"

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
} async_flash_t;

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

static ecat_foe_target_async_result_t flash_start(void *ctx,
                                                   ecat_foe_target_async_op_t *op)
{
    async_flash_t *f = (async_flash_t *)ctx;
    f->starts++;
    f->pending = true;
    if (f->immediate) return flash_poll(ctx, op);
    return ECAT_FOE_TARGET_ASYNC_PENDING;
}

static ecat_foe_target_async_result_t flash_poll(void *ctx,
                                                  ecat_foe_target_async_op_t *op)
{
    async_flash_t *f = (async_flash_t *)ctx;
    f->polls++;
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
    case ECAT_FOE_TARGET_OP_FINALIZE:
        if (op->target_id != ECAT_FOE_TARGET_MCU || op->fw_version != 0x01000000U)
            return ECAT_FOE_TARGET_ASYNC_FAILED;
        f->valid = true;
        break;
    }
    f->pending = false;
    return ECAT_FOE_TARGET_ASYNC_COMPLETE;
}

static void flash_abort(void *ctx)
{
    ((async_flash_t *)ctx)->pending = false;
}

static void flash_abort_session(void *ctx)
{
    async_flash_t *f = (async_flash_t *)ctx;
    f->pending = false;
    f->valid = false;
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

int main(void)
{
    CHECK(ecat_foe_core_async_ops() == ecat_foe_core_async_ssc_ops());
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
    memset(&cfg, 0, sizeof(cfg));
    cfg.project_id = 0x10010001U;
    cfg.project_name = "SERVO_DRV";
    cfg.hardware_id = 1U;
    cfg.local_hw_ver = 1U;
    cfg.max_header_size = ECAT_FOE_EFW_HEADER_MIN_LEN;
    cfg.targets[0].target_id = ECAT_FOE_TARGET_MCU;
    cfg.targets[0].enabled = true;
    cfg.targets[0].asynchronous = true;
    cfg.targets[0].async_ops = &flash_ops;
    cfg.targets[0].async_user_ctx = &flash;
    cfg.targets[0].max_payload_size = sizeof(flash.flash);
    cfg.targets[0].write_unit = 4U;
    CHECK(ecat_foe_core_init(&core, &cfg) == 0);
    CHECK(ecat_foe_core_async_init(&session, &core, async_packet,
          sizeof(async_packet), operation_storage, sizeof(operation_storage)) == 0);
    CHECK(ecat_foe_ssc_async_init(&gate, ecat_foe_core_async_ssc_ops(), &session,
          packet_storage, sizeof(packet_storage), 100U, 0x800AU,
          ECAT_FOE_SSC_ERR_PACKETNO, ECAT_FOE_SSC_ERR_PROGERROR) == 0);
    ecat_foe_ssc_init_async(&adapter, &core, &gate);

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
    CHECK(ecat_foe_core_deinit(&core) == 0);
    cfg.targets[0].write_unit = 256U;
    CHECK(ecat_foe_core_init(&core, &cfg) == 0);
    CHECK(ecat_foe_core_async_init(&session, &core, async_packet,
          sizeof(async_packet), operation_storage, sizeof(operation_storage)) == 0);
    CHECK(ecat_foe_ssc_async_init(&gate, ecat_foe_core_async_ssc_ops(), &session,
          packet_storage, sizeof(packet_storage), 100U, 0x800AU,
          ECAT_FOE_SSC_ERR_PACKETNO, ECAT_FOE_SSC_ERR_PROGERROR) == 0);
    ecat_foe_ssc_init_async(&adapter, &core, &gate);
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
    cfg.targets[0].write_unit = 4U;
    cfg.anti_rollback_enable = true;
    CHECK(ecat_foe_core_init(&core, &cfg) == 0);
    CHECK(ecat_foe_core_async_init(&session, &core, async_packet,
          sizeof(async_packet), operation_storage, sizeof(operation_storage)) == 0);
    CHECK(ecat_foe_ssc_async_init(&gate, ecat_foe_core_async_ssc_ops(), &session,
          packet_storage, sizeof(packet_storage), 100U, 0x800AU,
          ECAT_FOE_SSC_ERR_PACKETNO, ECAT_FOE_SSC_ERR_PROGERROR) == 0);
    ecat_foe_ssc_init_async(&adapter, &core, &gate);
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
    cfg.targets[0].asynchronous = false;
    cfg.targets[0].driver = &sync_flash.driver;
    cfg.targets[0].user_ctx = &sync_flash;
    cfg.targets[0].write_unit = 4U;
    cfg.targets[1].target_id = ECAT_FOE_TARGET_FPGA;
    cfg.targets[1].enabled = true;
    cfg.targets[1].asynchronous = true;
    cfg.targets[1].async_ops = &flash_ops;
    cfg.targets[1].async_user_ctx = &flash;
    cfg.targets[1].max_payload_size = sizeof(flash.flash);
    cfg.targets[1].write_unit = 4U;
    CHECK(ecat_foe_core_init(&core, &cfg) == 0);
    CHECK(ecat_foe_core_async_init(&session, &core, async_packet,
          sizeof(async_packet), operation_storage, sizeof(operation_storage)) == 0);
    CHECK(ecat_foe_core_on_wrq(&core, name, sizeof(name) - 1U) == 0);
    CHECK(ecat_foe_core_on_data(&core, efw, ECAT_FOE_EFW_HEADER_MIN_LEN) == 0);
    ecat_foe_core_on_abort(&core);
    CHECK(sync_flash.count_abort == 1U);
    CHECK(ecat_foe_core_on_wrq(&core, name, sizeof(name) - 1U) == 0);
    CHECK(ecat_foe_core_on_data(&core, efw, ECAT_FOE_EFW_HEADER_MIN_LEN) == 0);
    CHECK(ecat_foe_core_on_wrq(&core, name, sizeof(name) - 1U) == 0);
    CHECK(sync_flash.count_abort == 2U);
    CHECK(ecat_foe_core_on_data(&core, efw, ECAT_FOE_EFW_HEADER_MIN_LEN) == 0);
    CHECK(ecat_foe_core_deinit(&core) == 0);
    CHECK(sync_flash.count_abort == 3U);

    /* Generic ownership and explicit finish, with no SSC gate involved. */
    memset(&flash, 0, sizeof(flash));
    cfg.targets[0].asynchronous = true;
    CHECK(ecat_foe_core_init(&core, &cfg) == 0);
    CHECK(ecat_foe_core_async_init(&session, &core, async_packet,
          sizeof(async_packet), operation_storage, sizeof(operation_storage)) == 0);
    ecat_upgrade_t upgrade;
    int source = 0, contender = 0;
    uint64_t token = 0, rejected = 0;
    ecat_upgrade_request_t request = {ECAT_FOE_TARGET_MCU, 0x01000000U};
    CHECK(ecat_upgrade_init_async(&upgrade, &core, ecat_foe_core_async_ops(), &session) == 0);
    CHECK(ecat_upgrade_begin(&upgrade, &source, &request, &token) == ECAT_UPGRADE_COMPLETE);
    total = make_efw(efw, payload, sizeof(payload));
    for (size_t pos = 0; pos < total;) {
        size_t length = total - pos > 128U ? 128U : total - pos;
        memcpy(packet_storage, efw + pos, length);
        ecat_upgrade_result_t r = ecat_upgrade_write(&upgrade, &source, token, packet_storage, length);
        memset(packet_storage, 0xEE, sizeof(packet_storage));
        if (r == ECAT_UPGRADE_PENDING) {
            unsigned previous_starts = flash.starts;
            CHECK(ecat_upgrade_write(&upgrade, &source, token, efw, 1U) == ECAT_UPGRADE_BUSY);
            CHECK(ecat_upgrade_finish(&upgrade, &source, token) == ECAT_UPGRADE_BUSY);
            CHECK(ecat_upgrade_poll(&upgrade, &contender, token) == ECAT_UPGRADE_STALE);
            CHECK(flash.starts == previous_starts);
        }
        for (unsigned poll = 0; r == ECAT_UPGRADE_PENDING && poll < 64U; poll++)
            r = ecat_upgrade_poll(&upgrade, &source, token);
        CHECK(r == ECAT_UPGRADE_COMPLETE);
        pos += length;
    }
    CHECK(!flash.valid);
    CHECK(memcmp(flash.flash, payload, core.written_offset) == 0);
    CHECK(ecat_upgrade_begin(&upgrade, &contender, &request, &rejected) == ECAT_UPGRADE_BUSY);
    ecat_upgrade_result_t end = ecat_upgrade_finish(&upgrade, &source, token);
    CHECK(end == ECAT_UPGRADE_PENDING);
    for (unsigned poll = 0; end == ECAT_UPGRADE_PENDING && poll < 64U; poll++)
        end = ecat_upgrade_poll(&upgrade, &source, token);
    CHECK(end == ECAT_UPGRADE_COMPLETE && flash.valid);
    CHECK(memcmp(flash.flash, payload, sizeof(payload)) == 0);
    CHECK(ecat_upgrade_poll(&upgrade, &source, token) == ECAT_UPGRADE_STALE);
    CHECK(ecat_upgrade_begin(&upgrade, &contender, &request, &rejected) == ECAT_UPGRADE_COMPLETE);
    CHECK(ecat_upgrade_write(&upgrade, &contender, rejected, efw, 128U) == ECAT_UPGRADE_COMPLETE);
    CHECK(ecat_upgrade_write(&upgrade, &contender, rejected, efw + 128U, 20U) == ECAT_UPGRADE_PENDING);
    CHECK(ecat_upgrade_cancel(&upgrade, &source, rejected) == ECAT_UPGRADE_STALE);
    CHECK(ecat_upgrade_cancel(&upgrade, &contender, rejected) == ECAT_UPGRADE_COMPLETE);
    CHECK(flash.aborts == 1U && !flash.pending);
    CHECK(ecat_upgrade_cancel(&upgrade, &contender, rejected) == ECAT_UPGRADE_STALE);

    /* SSC gate uses the shared manager, including final retry after ownership
     * has passed to another transport following a successful commit. */
    ecat_foe_ssc_init_async(&adapter, &core, &gate);
    CHECK(ecat_foe_ssc_async_init(&gate, ecat_foe_transport_async_ops(), &adapter.transport,
          packet_storage, sizeof(packet_storage), 100U, 0x800AU,
          ECAT_FOE_SSC_ERR_PACKETNO, ECAT_FOE_SSC_ERR_PROGERROR) == 0);
    CHECK(ecat_foe_ssc_bind_upgrade(&adapter, &upgrade) == 0);
    CHECK(ecat_upgrade_begin(&upgrade, &source, &request, &token) == ECAT_UPGRADE_COMPLETE);
    CHECK(ecat_foe_ssc_on_write(&adapter, name, sizeof(name) - 1U, 0U) != 0U);
    ecat_foe_ssc_on_abort(&adapter, 0U);
    CHECK(ecat_upgrade_session_owns(&upgrade.session, &source, token));
    CHECK(ecat_upgrade_cancel(&upgrade, &source, token) == ECAT_UPGRADE_COMPLETE);
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
                CHECK(ecat_upgrade_begin(&upgrade, &source, &request, &token) == ECAT_UPGRADE_COMPLETE);
            }
            r = ecat_foe_ssc_on_data_following(&adapter, efw + pos, (uint16_t)length, more, tick++);
        }
        CHECK(r == 0U);
        pos += length;
    }
    CHECK(ecat_upgrade_session_owns(&upgrade.session, &source, token));
    ecat_foe_ssc_on_abort(&adapter, 0U);
    CHECK(ecat_upgrade_session_owns(&upgrade.session, &source, token));
    CHECK(ecat_upgrade_cancel(&upgrade, &source, token) == ECAT_UPGRADE_COMPLETE);

    CHECK(ecat_foe_ssc_on_write(&adapter, name, sizeof(name) - 1U, 0U) == 0U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, efw, 128U, true, tick++) == 0U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, efw + 128U, 20U, true, tick++) == gate.busy_foe_code);
    tick += 100U;
    CHECK(ecat_foe_ssc_on_main_loop(&adapter, tick++) == gate.operation_error_foe_code);
    CHECK(ecat_upgrade_begin(&upgrade, &source, &request, &token) == ECAT_UPGRADE_COMPLETE);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, efw + 128U, 20U, true, tick++) == gate.operation_error_foe_code);
    CHECK(ecat_upgrade_session_owns(&upgrade.session, &source, token));
    CHECK(ecat_upgrade_cancel(&upgrade, &source, token) == ECAT_UPGRADE_COMPLETE);

    puts("PASS: async core header split, SSC BUSY/retry, Flash write/readback and finalize");
    return 0;
}
