#include "ecat_upgrade_file.h"
#include <string.h>

int ecat_upgrade_file_init(ecat_upgrade_file_t *f, ecat_upgrade_t *upgrade,
    uint8_t *buffer, size_t capacity)
{
    if (f == NULL || upgrade == NULL || upgrade->core == NULL || buffer == NULL || capacity == 0U)
        return -1;
    memset(f, 0, sizeof(*f));
    f->upgrade = upgrade;
    f->transfer = NULL;
    f->buffer = buffer;
    f->capacity = capacity;
    f->result = ECAT_UPGRADE_INVALID;
    return 0;
}

int ecat_upgrade_file_init_transfer(ecat_upgrade_file_t *f, ecat_transfer_t *transfer,
    uint8_t *buffer, size_t capacity)
{
    if (f == NULL || transfer == NULL || buffer == NULL || capacity == 0U)
        return -1;
    memset(f, 0, sizeof(*f));
    f->upgrade = NULL;
    f->transfer = transfer;
    f->buffer = buffer;
    f->capacity = capacity;
    f->result = ECAT_UPGRADE_INVALID;
    return 0;
}

static ecat_upgrade_result_t map_transfer_res(ecat_transfer_result_t res)
{
    switch (res) {
    case ECAT_TRANSFER_COMPLETE: return ECAT_UPGRADE_COMPLETE;
    case ECAT_TRANSFER_PENDING:  return ECAT_UPGRADE_PENDING;
    case ECAT_TRANSFER_BUSY:     return ECAT_UPGRADE_BUSY;
    case ECAT_TRANSFER_STALE:    return ECAT_UPGRADE_STALE;
    case ECAT_TRANSFER_INVALID:  return ECAT_UPGRADE_INVALID;
    default:                     return ECAT_UPGRADE_FAILED;
    }
}

ecat_upgrade_result_t ecat_upgrade_file_begin(ecat_upgrade_file_t *f,
    FILE *stream, const ecat_upgrade_request_t *request)
{
    if (f == NULL || (f->upgrade == NULL && f->transfer == NULL) || stream == NULL || request == NULL)
        return ECAT_UPGRADE_INVALID;
    if (f->active) return ECAT_UPGRADE_BUSY;

    ecat_upgrade_result_t result;
    if (f->transfer != NULL) {
        ecat_transfer_request_t treq;
        memset(&treq, 0, sizeof(treq));
        treq.transport_id = 2U; /* FILE 通道 ID */
        treq.target_id = request->target_id;
        treq.direction = ECAT_TRANSFER_DIR_IMPORT;
        treq.expected_size = 0U; /* 流式直至 EOF */
        treq.fw_version = request->fw_version;
        result = map_transfer_res(ecat_transfer_begin(f->transfer, f, &treq, &f->token));
    } else {
        result = ecat_upgrade_begin(f->upgrade, f, request, &f->token);
    }

    if (result == ECAT_UPGRADE_PENDING) {
        f->stream = stream;
        f->active = true;
        f->pending = true;
        f->ending = false;
        f->io_error = false;
        f->result = ECAT_UPGRADE_PENDING;
        return ECAT_UPGRADE_PENDING;
    }

    if (result != ECAT_UPGRADE_COMPLETE) return result;
    f->stream = stream;
    f->active = true;
    f->pending = false;
    f->ending = false;
    f->io_error = false;
    f->result = ECAT_UPGRADE_PENDING;
    return ECAT_UPGRADE_COMPLETE;
}

ecat_upgrade_result_t ecat_upgrade_file_cancel(ecat_upgrade_file_t *f)
{
    if (f == NULL || (f->upgrade == NULL && f->transfer == NULL)) return ECAT_UPGRADE_INVALID;
    if (!f->active) return ECAT_UPGRADE_STALE;

    ecat_upgrade_result_t result;
    if (f->transfer != NULL) {
        result = map_transfer_res(ecat_transfer_cancel(f->transfer, f, f->token));
    } else {
        result = ecat_upgrade_cancel(f->upgrade, f, f->token);
    }
    f->active = false;
    f->pending = false;
    f->result = ECAT_UPGRADE_FAILED;
    return result;
}

static ecat_upgrade_result_t advance(ecat_upgrade_file_t *f, ecat_upgrade_result_t result)
{
    f->pending = result == ECAT_UPGRADE_PENDING;
    if (result != ECAT_UPGRADE_COMPLETE && result != ECAT_UPGRADE_PENDING) {
        (void)ecat_upgrade_file_cancel(f);
        return ECAT_UPGRADE_FAILED;
    }
    if (result == ECAT_UPGRADE_COMPLETE && f->ending) {
        f->active = false;
        f->result = ECAT_UPGRADE_COMPLETE;
    }
    return f->result;
}

ecat_upgrade_result_t ecat_upgrade_file_step(ecat_upgrade_file_t *f)
{
    if (f == NULL || (f->upgrade == NULL && f->transfer == NULL)) return ECAT_UPGRADE_INVALID;
    if (!f->active) return f->result;

    if (f->pending) {
        ecat_upgrade_result_t poll_res = (f->transfer != NULL)
            ? map_transfer_res(ecat_transfer_poll(f->transfer, f, f->token))
            : ecat_upgrade_poll(f->upgrade, f, f->token);
        return advance(f, poll_res);
    }

    size_t length = fread(f->buffer, 1U, f->capacity, f->stream);
    if (ferror(f->stream)) {
        f->io_error = true;
        (void)ecat_upgrade_file_cancel(f);
        return ECAT_UPGRADE_FAILED;
    }

    if (length != 0U) {
        ecat_upgrade_result_t write_res = (f->transfer != NULL)
            ? map_transfer_res(ecat_transfer_write(f->transfer, f, f->token, f->buffer, length))
            : ecat_upgrade_write(f->upgrade, f, f->token, f->buffer, length);
        return advance(f, write_res);
    }

    if (!feof(f->stream)) {
        f->io_error = true;
        (void)ecat_upgrade_file_cancel(f);
        return ECAT_UPGRADE_FAILED;
    }

    f->ending = true;
    ecat_upgrade_result_t fin_res = (f->transfer != NULL)
        ? map_transfer_res(ecat_transfer_finish(f->transfer, f, f->token))
        : ecat_upgrade_finish(f->upgrade, f, f->token);
    return advance(f, fin_res);
}
