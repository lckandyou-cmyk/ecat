#include "ecat_foe_core_async.h"

#include <string.h>

static int validate_header(ecat_foe_core_async_t *s);
static ecat_foe_ssc_async_result_t pump(ecat_foe_core_async_t *s);
static int begin_operation(ecat_foe_core_async_t *s,
                           ecat_foe_target_op_kind_t kind,
                           uint32_t offset,
                           uint32_t need_end,
                           const uint8_t *data,
                           uint32_t length,
                           uint8_t *read_destination,
                           uint32_t *erased_end);
static int complete_operation(ecat_foe_core_async_t *s);
static int pump_payload(ecat_foe_core_async_t *s);
static int pump_end(ecat_foe_core_async_t *s);
static ecat_foe_core_err_t phase_error(ecat_foe_core_async_phase_t phase);
static ecat_foe_ssc_async_result_t fail_session(ecat_foe_core_async_t *s,
                                                 ecat_foe_core_err_t error);

static ecat_foe_ssc_async_result_t async_start(void *ctx, const uint8_t *data,
                                                size_t length, bool following);
static ecat_foe_ssc_async_result_t async_poll(void *ctx);
static void async_abort(void *ctx);

static const ecat_foe_ssc_async_ops_t s_ssc_ops = {
    async_start,
    async_poll,
    async_abort
};

const ecat_foe_ssc_async_ops_t *ecat_foe_core_async_ssc_ops(void)
{
    return &s_ssc_ops;
}

int ecat_foe_core_async_init(ecat_foe_core_async_t *s,
                             ecat_foe_core_t *core,
                             uint8_t *packet_storage,
                             size_t packet_capacity,
                             uint8_t *operation_storage,
                             size_t operation_capacity)
{
    if (s == NULL || core == NULL || !core->is_configured ||
        packet_storage == NULL || packet_capacity == 0U ||
        operation_storage == NULL || packet_storage == operation_storage ||
        operation_capacity < sizeof(s->readback)) {
        return -1;
    }
    memset(s, 0, sizeof(*s));
    s->core = core;
    s->packet = packet_storage;
    s->packet_capacity = packet_capacity;
    s->operation_storage = operation_storage;
    s->operation_capacity = operation_capacity;
    s->state = ECAT_FOE_CORE_ASYNC_IDLE;
    core->async_abort_hook = async_abort;
    core->async_abort_ctx = s;
    return 0;
}

static ecat_foe_ssc_async_result_t async_start(void *ctx, const uint8_t *data,
                                                size_t length, bool following)
{
    ecat_foe_core_async_t *s = (ecat_foe_core_async_t *)ctx;
    if (s == NULL || s->core == NULL ||
        (s->core->state != ECAT_FOE_STATE_WRQ_ACKED &&
         s->core->state != ECAT_FOE_STATE_DATA) ||
        length > s->packet_capacity || (length > 0U && data == NULL)) {
        return ECAT_FOE_SSC_ASYNC_FAILED;
    }
    if (s->core->state == ECAT_FOE_STATE_WRQ_ACKED &&
        s->core->header_rx_len == 0U && !s->core->header_complete) {
        s->state = ECAT_FOE_CORE_ASYNC_IDLE;
        s->phase = ECAT_FOE_CORE_ASYNC_PHASE_NONE;
        s->target_session_started = false;
        s->target_session_aborted = false;
        s->readback_offset = 0U;
        s->readback_remaining = 0U;
        s->readback_crc = 0U;
    }
    if (s->runner.failed && ecat_foe_target_async_reset(&s->runner) != 0) {
        return ECAT_FOE_SSC_ASYNC_FAILED;
    }
    if (length > 0U) {
        memcpy(s->packet, data, length);
    }
    s->packet_length = length;
    s->packet_cursor = 0U;
    s->packet_payload_start = 0U;
    s->packet_following = following;
    s->packet_complete = false;
    s->packet_accounted = false;
    s->end_requested = !following;
    s->state = ECAT_FOE_CORE_ASYNC_PACKET;

    return pump(s);
}

static ecat_foe_ssc_async_result_t async_poll(void *ctx)
{
    ecat_foe_core_async_t *s = (ecat_foe_core_async_t *)ctx;
    if (s == NULL || s->core == NULL) {
        return ECAT_FOE_SSC_ASYNC_FAILED;
    }
    if (s->runner.pending) {
        ecat_foe_target_async_result_t r = ecat_foe_target_async_poll(&s->runner);
        if (r == ECAT_FOE_TARGET_ASYNC_PENDING) {
            return ECAT_FOE_SSC_ASYNC_PENDING;
        }
        if (r == ECAT_FOE_TARGET_ASYNC_FAILED) {
            return fail_session(s, phase_error(s->phase));
        }
        int done = complete_operation(s);
        if (done < 0) {
            return fail_session(s, s->core->last_error != ECAT_FOE_CORE_OK
                ? s->core->last_error : phase_error(s->phase));
        }
        if (done > 0) return ECAT_FOE_SSC_ASYNC_PENDING;
        if (s->core->state == ECAT_FOE_STATE_SUCCESS) {
            return ECAT_FOE_SSC_ASYNC_COMPLETE;
        }
    }
    return pump(s);
}

static ecat_foe_core_err_t phase_error(ecat_foe_core_async_phase_t phase)
{
    switch (phase) {
    case ECAT_FOE_CORE_ASYNC_PHASE_INIT: return ECAT_FOE_CORE_ERR_DRV_INIT;
    case ECAT_FOE_CORE_ASYNC_PHASE_INVALIDATE: return ECAT_FOE_CORE_ERR_DRV_INVALIDATE;
    case ECAT_FOE_CORE_ASYNC_PHASE_ERASE: return ECAT_FOE_CORE_ERR_DRV_ERASE;
    case ECAT_FOE_CORE_ASYNC_PHASE_WRITE: return ECAT_FOE_CORE_ERR_DRV_WRITE;
    case ECAT_FOE_CORE_ASYNC_PHASE_READ: return ECAT_FOE_CORE_ERR_DRV_READ;
    case ECAT_FOE_CORE_ASYNC_PHASE_FINALIZE: return ECAT_FOE_CORE_ERR_DRV_FINALIZE;
    default: return ECAT_FOE_CORE_ERR_INVALID_STATE;
    }
}

static ecat_foe_ssc_async_result_t fail_session(ecat_foe_core_async_t *s,
                                                 ecat_foe_core_err_t error)
{
    ecat_foe_core_t *c = s->core;
    if (s->target_session_started && !s->target_session_aborted) {
        ecat_foe_target_async_abort_session(&s->runner);
        s->target_session_aborted = true;
    } else if (s->runner.pending) {
        ecat_foe_target_async_abort(&s->runner);
    }
    if (c->signature_active && c->cfg.signature_verifier.abort != NULL) {
        c->cfg.signature_verifier.abort(c->cfg.signature_verifier.user_ctx);
        c->signature_active = false;
    }
    c->session_active = false; /* async driver session was aborted above */
    c->state = ECAT_FOE_STATE_ERROR;
    c->last_error = error;
    s->phase = ECAT_FOE_CORE_ASYNC_PHASE_NONE;
    s->state = ECAT_FOE_CORE_ASYNC_FAILED;
    return ECAT_FOE_SSC_ASYNC_FAILED;
}

/* Returns 1 for pending, 0 for complete, and -1 for failure. */
static int begin_operation(ecat_foe_core_async_t *s,
                           ecat_foe_target_op_kind_t kind,
                           uint32_t offset,
                           uint32_t need_end,
                           const uint8_t *data,
                           uint32_t length,
                           uint8_t *read_destination,
                           uint32_t *erased_end)
{
    ecat_foe_core_async_phase_t phase;
    switch (kind) {
    case ECAT_FOE_TARGET_OP_INIT: phase = ECAT_FOE_CORE_ASYNC_PHASE_INIT; break;
    case ECAT_FOE_TARGET_OP_INVALIDATE: phase = ECAT_FOE_CORE_ASYNC_PHASE_INVALIDATE; break;
    case ECAT_FOE_TARGET_OP_ERASE_TO: phase = ECAT_FOE_CORE_ASYNC_PHASE_ERASE; break;
    case ECAT_FOE_TARGET_OP_WRITE: phase = ECAT_FOE_CORE_ASYNC_PHASE_WRITE; break;
    case ECAT_FOE_TARGET_OP_READ: phase = ECAT_FOE_CORE_ASYNC_PHASE_READ; break;
    default: phase = ECAT_FOE_CORE_ASYNC_PHASE_FINALIZE; break;
    }
    s->phase = phase;
    ecat_foe_target_async_result_t r = ecat_foe_target_async_start(
        &s->runner, kind, offset, need_end, data, length, read_destination, erased_end);
    if (r == ECAT_FOE_TARGET_ASYNC_PENDING) {
        if (kind == ECAT_FOE_TARGET_OP_INIT) s->target_session_started = true;
        return 1;
    }
    if (r == ECAT_FOE_TARGET_ASYNC_FAILED) {
        s->core->last_error = phase_error(phase);
        return -1;
    }
    if (kind == ECAT_FOE_TARGET_OP_INIT) s->target_session_started = true;
    return complete_operation(s);
}

static int validate_header(ecat_foe_core_async_t *s)
{
    ecat_foe_core_t *c = s->core;
    ecat_foe_efw_config_t cfg;
    if (ecat_foe_efw_parse_stage2(c->header_buf, c->header_total_len, &c->hdr) != ECAT_FOE_EFW_OK) {
        c->last_error = ECAT_FOE_CORE_ERR_HDR_CRC;
        return -1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.project_id = c->cfg.project_id;
    cfg.hardware_id = c->cfg.hardware_id;
    cfg.local_hw_ver = c->cfg.local_hw_ver;
    cfg.anti_rollback_enable = c->cfg.anti_rollback_enable;
    cfg.signature_verify_enable = c->cfg.signature_verify_enable;
    for (size_t i = 0; i < ECAT_FOE_TARGET_COUNT; i++) {
        cfg.targets[i].enabled = c->cfg.targets[i].enabled;
        cfg.targets[i].max_payload_size = c->cfg.targets[i].max_payload_size;
    }
    if (c->hdr.target_id < 1U || c->hdr.target_id > ECAT_FOE_TARGET_COUNT) {
        c->last_error = ECAT_FOE_CORE_ERR_HDR_VERIFY;
        return -1;
    }
    if (c->cfg.anti_rollback_enable) {
        uint32_t floor = 0U;
        if (port_get_version_floor(c->hdr.target_id, &floor) != 0) {
            c->last_error = ECAT_FOE_CORE_ERR_ANTI_ROLLBACK_GET;
            return -1;
        }
        cfg.targets[c->hdr.target_id - 1U].version_floor = floor;
    }
    if (ecat_foe_efw_parse_stage3(&c->hdr, &cfg) != ECAT_FOE_EFW_OK) {
        c->last_error = ECAT_FOE_CORE_ERR_HDR_VERIFY;
        return -1;
    }
    if (c->hdr.target_id != c->req_target_id || c->hdr.fw_version != c->req_fw_version) {
        c->last_error = ECAT_FOE_CORE_ERR_HDR_CROSS_MISMATCH;
        return -1;
    }
    const ecat_foe_target_desc_t *target = &c->cfg.targets[c->hdr.target_id - 1U];
    if (!target->enabled || c->hdr.payload_length > target->max_payload_size) {
        c->last_error = ECAT_FOE_CORE_ERR_PAYLOAD_LIMIT_EXCEEDED;
        return -1;
    }
    if (!target->asynchronous || target->async_ops == NULL) {
        c->last_error = ECAT_FOE_CORE_ERR_DRV_NOT_REGISTERED;
        return -1;
    }
    if (ecat_foe_target_async_init(&s->runner, target->async_ops,
            target->async_user_ctx, s->operation_storage,
            s->operation_capacity) != 0) {
        c->last_error = ECAT_FOE_CORE_ERR_CFG_TARGET;
        return -1;
    }
    if (c->cfg.signature_verify_enable) {
        ecat_foe_signature_verifier_t *v = &c->cfg.signature_verifier;
        if (!c->hdr.signed_package) {
            c->last_error = ECAT_FOE_CORE_ERR_SIGNATURE_REQUIRED;
            return -1;
        }
        if (v->begin(c->hdr.key_id, c->hdr.signature, v->user_ctx) != 0) {
            v->abort(v->user_ctx);
            c->last_error = ECAT_FOE_CORE_ERR_SIGNATURE_UNAVAILABLE;
            return -1;
        }
        c->signature_active = true;
        if (v->update((const uint8_t *)ECAT_FOE_SIGNATURE_DOMAIN,
                      ECAT_FOE_SIGNATURE_DOMAIN_LEN, v->user_ctx) != 0 ||
            v->update(c->header_buf, 80U, v->user_ctx) != 0) {
            c->last_error = ECAT_FOE_CORE_ERR_SIGNATURE_INVALID;
            return -1;
        }
    }
    c->active_drv = NULL;
    c->active_user_ctx = target->async_user_ctx;
    c->active_write_unit = target->write_unit;
    c->header_complete = true;
    c->state = ECAT_FOE_STATE_DATA;
    c->total_rx_bytes = c->header_total_len;
    c->payload_rx_bytes = 0U;
    c->written_offset = 0U;
    c->erased_watermark = 0U;
    c->residual_len = 0U;
    ecat_foe_crc32_init(&c->stream_crc);
    s->packet_payload_start = c->header_total_len;
    return 0;
}

static int complete_operation(ecat_foe_core_async_t *s)
{
    ecat_foe_core_t *c = s->core;
    ecat_foe_core_async_phase_t phase = s->phase;
    s->phase = ECAT_FOE_CORE_ASYNC_PHASE_NONE;
    switch (phase) {
    case ECAT_FOE_CORE_ASYNC_PHASE_INIT:
        c->session_active = true;
        c->driver_abort_called = false;
        c->driver_finalized = false;
        c->target_incomplete_mask |= (1U << (c->hdr.target_id - 1U));
        return begin_operation(s, ECAT_FOE_TARGET_OP_INVALIDATE, 0U, 0U,
                               NULL, 0U, NULL, NULL);
    case ECAT_FOE_CORE_ASYNC_PHASE_INVALIDATE:
        c->driver_invalidated = true;
        return 0;
    case ECAT_FOE_CORE_ASYNC_PHASE_ERASE:
        if (c->erased_watermark < s->operation_need_end ||
            c->erased_watermark >
                c->cfg.targets[c->hdr.target_id - 1U].max_payload_size) {
            c->last_error = ECAT_FOE_CORE_ERR_DRV_ERASE;
            return -1;
        }
        return begin_operation(s, ECAT_FOE_TARGET_OP_WRITE,
                               s->operation_offset, 0U,
                               s->operation_storage, s->operation_length,
                               NULL, NULL);
    case ECAT_FOE_CORE_ASYNC_PHASE_WRITE:
        c->written_offset += s->operation_length;
        return 0;
    case ECAT_FOE_CORE_ASYNC_PHASE_READ:
        ecat_foe_crc32_update(&s->readback_crc, s->readback, s->readback_chunk);
        if (c->cfg.signature_verify_enable &&
            c->cfg.signature_verifier.update(s->readback, s->readback_chunk,
                                             c->cfg.signature_verifier.user_ctx) != 0) {
            c->last_error = ECAT_FOE_CORE_ERR_SIGNATURE_INVALID;
            return -1;
        }
        s->readback_offset += s->readback_chunk;
        s->readback_remaining -= s->readback_chunk;
        return 0;
    case ECAT_FOE_CORE_ASYNC_PHASE_FINALIZE:
        c->target_incomplete_mask &= ~(1U << (c->req_target_id - 1U));
        c->driver_finalized = true;
        c->driver_invalidated = false;
        c->session_active = false;
        s->target_session_started = false;
        c->state = ECAT_FOE_STATE_SUCCESS;
        c->last_error = ECAT_FOE_CORE_OK;
        s->state = ECAT_FOE_CORE_ASYNC_IDLE;
        s->packet_complete = true;
        return 0;
    default:
        return -1;
    }
}

static int start_aligned_write(ecat_foe_core_async_t *s,
                               const uint8_t *data,
                               uint32_t length)
{
    ecat_foe_core_t *c = s->core;
    if (length == 0U || length > s->operation_capacity ||
        (length % c->active_write_unit) != 0U ||
        c->written_offset > UINT32_MAX - length) {
        c->last_error = ECAT_FOE_CORE_ERR_INVALID_ARG;
        return -1;
    }
    s->operation_offset = c->written_offset;
    s->operation_length = length;
    s->operation_need_end = c->written_offset + length;
    if (s->operation_need_end > c->cfg.targets[c->hdr.target_id - 1U].max_payload_size) {
        c->last_error = ECAT_FOE_CORE_ERR_PAYLOAD_LIMIT_EXCEEDED;
        return -1;
    }
    if (data != s->operation_storage) {
        memcpy(s->operation_storage, data, length);
    }
    if (s->operation_need_end > c->erased_watermark) {
        return begin_operation(s, ECAT_FOE_TARGET_OP_ERASE_TO, 0U,
                               s->operation_need_end, NULL, 0U, NULL,
                               &c->erased_watermark);
    }
    return begin_operation(s, ECAT_FOE_TARGET_OP_WRITE, s->operation_offset,
                           0U, s->operation_storage, length, NULL, NULL);
}

static int pump_payload(ecat_foe_core_async_t *s)
{
    ecat_foe_core_t *c = s->core;
    uint32_t unit = c->active_write_unit;
    if (unit == 0U || unit > sizeof(c->residual_buf) || unit > s->operation_capacity) {
        c->last_error = ECAT_FOE_CORE_ERR_INVALID_ARG;
        return -1;
    }
    if (!s->packet_accounted) {
        size_t start = s->packet_cursor;
        size_t len = s->packet_length - start;
        uint64_t total = (uint64_t)c->payload_rx_bytes + len;
        if (total > c->hdr.payload_length) {
            c->last_error = ECAT_FOE_CORE_ERR_OVER_RECEIVE;
            return -1;
        }
        ecat_foe_crc32_update(&c->stream_crc, s->packet + start, len);
        c->payload_rx_bytes += (uint32_t)len;
        c->total_rx_bytes += len;
        s->packet_accounted = true;
    }
    while (s->packet_cursor < s->packet_length) {
        size_t remaining = s->packet_length - s->packet_cursor;
        if (c->residual_len != 0U) {
            size_t fill = unit - c->residual_len;
            if (fill > remaining) fill = remaining;
            memcpy(c->residual_buf + c->residual_len,
                   s->packet + s->packet_cursor, fill);
            c->residual_len += (uint32_t)fill;
            s->packet_cursor += fill;
            if (c->residual_len == unit) {
                memcpy(s->operation_storage, c->residual_buf, unit);
                c->residual_len = 0U;
                int r = start_aligned_write(s, s->operation_storage, unit);
                if (r != 0) return r;
            }
        } else if (remaining >= unit) {
            uint32_t n = (uint32_t)(remaining - (remaining % unit));
            if (n > s->operation_capacity) {
                n = (uint32_t)(s->operation_capacity - (s->operation_capacity % unit));
            }
            if (n == 0U) {
                c->last_error = ECAT_FOE_CORE_ERR_CFG_HEADER_BUF;
                return -1;
            }
            const uint8_t *p = s->packet + s->packet_cursor;
            s->packet_cursor += n;
            int r = start_aligned_write(s, p, n);
            if (r != 0) return r;
        } else {
            memcpy(c->residual_buf, s->packet + s->packet_cursor, remaining);
            c->residual_len = (uint32_t)remaining;
            s->packet_cursor = s->packet_length;
        }
    }
    s->packet_complete = true;
    if (s->end_requested) {
        s->state = ECAT_FOE_CORE_ASYNC_ENDING;
        return pump_end(s);
    }
    return 0;
}

static int pump_end(ecat_foe_core_async_t *s)
{
    ecat_foe_core_t *c = s->core;
    if (c->payload_rx_bytes != c->hdr.payload_length) {
        c->last_error = ECAT_FOE_CORE_ERR_UNDER_RECEIVE;
        return -1;
    }
    if (c->residual_len != 0U) {
        uint32_t unit = c->active_write_unit;
        if (c->residual_len >= unit || unit > sizeof(c->residual_buf)) {
            c->last_error = ECAT_FOE_CORE_ERR_INVALID_ARG;
            return -1;
        }
        memset(c->residual_buf + c->residual_len, 0xFF, unit - c->residual_len);
        memcpy(s->operation_storage, c->residual_buf, unit);
        c->residual_len = 0U;
        return start_aligned_write(s, s->operation_storage, unit);
    }
    if (ecat_foe_crc32_final(&c->stream_crc) != c->hdr.payload_crc32) {
        c->last_error = ECAT_FOE_CORE_ERR_STREAM_CRC_MISMATCH;
        return -1;
    }
    if (s->readback_offset == 0U && s->readback_remaining == 0U) {
        ecat_foe_crc32_init(&s->readback_crc);
        s->readback_remaining = c->hdr.payload_length;
    }
    if (s->readback_remaining != 0U) {
        uint32_t n = s->readback_remaining < sizeof(s->readback)
            ? s->readback_remaining : (uint32_t)sizeof(s->readback);
        s->readback_chunk = n;
        return begin_operation(s, ECAT_FOE_TARGET_OP_READ, s->readback_offset,
                               0U, NULL, n, s->readback, NULL);
    }
    if (ecat_foe_crc32_final(&s->readback_crc) != c->hdr.payload_crc32) {
        c->last_error = ECAT_FOE_CORE_ERR_READBACK_CRC_MISMATCH;
        return -1;
    }
    if (c->cfg.signature_verify_enable) {
        if (!c->signature_active || c->cfg.signature_verifier.finish(
                c->cfg.signature_verifier.user_ctx) != 0) {
            c->last_error = ECAT_FOE_CORE_ERR_SIGNATURE_INVALID;
            return -1;
        }
        c->signature_active = false;
    }
    ecat_foe_target_async_result_t final_result = ecat_foe_target_async_start_finalize(
        &s->runner, c->hdr.target_id, c->hdr.fw_version,
        c->hdr.min_allow_version, c->cfg.anti_rollback_enable);
    s->phase = ECAT_FOE_CORE_ASYNC_PHASE_FINALIZE;
    if (final_result == ECAT_FOE_TARGET_ASYNC_PENDING) return 1;
    if (final_result == ECAT_FOE_TARGET_ASYNC_FAILED) {
        c->last_error = ECAT_FOE_CORE_ERR_DRV_FINALIZE;
        return -1;
    }
    return complete_operation(s);
}

static ecat_foe_ssc_async_result_t pump(ecat_foe_core_async_t *s)
{
    ecat_foe_core_t *c = s->core;
    if (s->state == ECAT_FOE_CORE_ASYNC_FAILED) return ECAT_FOE_SSC_ASYNC_FAILED;
    if (s->state == ECAT_FOE_CORE_ASYNC_ENDING) {
        int r = pump_end(s);
        if (r < 0) return fail_session(s, c->last_error);
        return r > 0 ? ECAT_FOE_SSC_ASYNC_PENDING :
            (c->state == ECAT_FOE_STATE_SUCCESS ? ECAT_FOE_SSC_ASYNC_COMPLETE
                                                : ECAT_FOE_SSC_ASYNC_PENDING);
    }
    if (s->state != ECAT_FOE_CORE_ASYNC_PACKET) return ECAT_FOE_SSC_ASYNC_FAILED;

    if (!c->header_complete) {
        while (s->packet_cursor < s->packet_length &&
               c->header_rx_len < ECAT_FOE_EFW_STAGE1_PROBE_LEN) {
            c->header_buf[c->header_rx_len++] = s->packet[s->packet_cursor++];
        }
        if (c->header_rx_len >= ECAT_FOE_EFW_STAGE1_PROBE_LEN &&
            c->header_total_len == 0U) {
            if (ecat_foe_efw_parse_stage1(c->header_buf, c->header_rx_len,
                    c->cfg.max_header_size, &c->header_total_len) != ECAT_FOE_EFW_OK) {
                c->last_error = ECAT_FOE_CORE_ERR_HDR_PROBE;
                return fail_session(s, c->last_error);
            }
        }
        while (c->header_total_len != 0U && s->packet_cursor < s->packet_length &&
               c->header_rx_len < c->header_total_len) {
            c->header_buf[c->header_rx_len++] = s->packet[s->packet_cursor++];
        }
        if (c->header_total_len == 0U || c->header_rx_len < c->header_total_len) {
            if (s->end_requested) {
                c->last_error = ECAT_FOE_CORE_ERR_UNDER_RECEIVE;
                return fail_session(s, c->last_error);
            }
            s->packet_complete = true;
            return ECAT_FOE_SSC_ASYNC_COMPLETE;
        }
        if (validate_header(s) != 0) return fail_session(s, c->last_error);
        if (s->packet_length - s->packet_cursor > c->hdr.payload_length) {
            c->last_error = ECAT_FOE_CORE_ERR_OVER_RECEIVE;
            return fail_session(s, c->last_error);
        }
        int r = begin_operation(s, ECAT_FOE_TARGET_OP_INIT, 0U, 0U,
                                NULL, 0U, NULL, NULL);
        if (r < 0) return fail_session(s, ECAT_FOE_CORE_ERR_DRV_INIT);
        if (r > 0) return ECAT_FOE_SSC_ASYNC_PENDING;
        /* init completion started invalidate; if it is still pending, return. */
        if (s->runner.pending) return ECAT_FOE_SSC_ASYNC_PENDING;
    }

    if (!c->driver_invalidated) {
        /* This also covers synchronous-complete init -> invalidate chains. */
        if (s->phase != ECAT_FOE_CORE_ASYNC_PHASE_NONE || s->runner.pending) {
            return ECAT_FOE_SSC_ASYNC_PENDING;
        }
        int r = begin_operation(s, ECAT_FOE_TARGET_OP_INVALIDATE, 0U, 0U,
                                NULL, 0U, NULL, NULL);
        if (r < 0) return fail_session(s, ECAT_FOE_CORE_ERR_DRV_INVALIDATE);
        if (r > 0) return ECAT_FOE_SSC_ASYNC_PENDING;
        c->driver_invalidated = true;
        c->session_active = true;
        c->target_incomplete_mask |= (1U << (c->hdr.target_id - 1U));
    }

    int r = pump_payload(s);
    if (r < 0) return fail_session(s, c->last_error);
    if (r > 0) return ECAT_FOE_SSC_ASYNC_PENDING;
    if (s->runner.pending) return ECAT_FOE_SSC_ASYNC_PENDING;
    if (s->state == ECAT_FOE_CORE_ASYNC_ENDING) return pump(s);
    return s->packet_complete ? ECAT_FOE_SSC_ASYNC_COMPLETE : ECAT_FOE_SSC_ASYNC_PENDING;
}

static void async_abort(void *ctx)
{
    ecat_foe_core_async_t *s = (ecat_foe_core_async_t *)ctx;
    if (s == NULL) {
        return;
    }
    if (s->runner.ops != NULL && s->target_session_started &&
        !s->target_session_aborted) {
        ecat_foe_target_async_abort_session(&s->runner);
        s->target_session_aborted = true;
    } else if (s->runner.pending) {
        ecat_foe_target_async_abort(&s->runner);
    }
    if (s->core != NULL) {
        s->core->session_active = false;
        s->core->driver_abort_called = true;
    }
    s->state = ECAT_FOE_CORE_ASYNC_IDLE;
    s->phase = ECAT_FOE_CORE_ASYNC_PHASE_NONE;
    s->packet_length = 0U;
    s->packet_cursor = 0U;
    s->packet_complete = false;
    s->end_requested = false;
}

void ecat_foe_core_async_abort(ecat_foe_core_async_t *s)
{
    if (s != NULL && s->core != NULL) {
        ecat_foe_core_on_abort(s->core);
    }
}
