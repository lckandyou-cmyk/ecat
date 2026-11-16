#include "ecat_transfer_file.h"
#include <string.h>

int ecat_transfer_file_init(ecat_transfer_file_t *f, ecat_transfer_t *transfer,
                            uint8_t *buffer, size_t capacity)
{
    if (f == NULL || transfer == NULL || buffer == NULL || capacity == 0U)
        return -1;
    memset(f, 0, sizeof(*f));
    f->transfer = transfer;
    f->buffer = buffer;
    f->capacity = capacity;
    f->result = ECAT_TRANSFER_INVALID;
    return 0;
}

ecat_transfer_result_t ecat_transfer_file_begin(ecat_transfer_file_t *f,
    FILE *stream, const ecat_transfer_request_t *request)
{
    if (f == NULL || f->transfer == NULL || stream == NULL || request == NULL ||
        request->direction != ECAT_TRANSFER_DIR_IMPORT)
        return ECAT_TRANSFER_INVALID;
    if (f->active) return ECAT_TRANSFER_BUSY;

    ecat_transfer_result_t result = ecat_transfer_begin(f->transfer, f, request, &f->token);
    if (result != ECAT_TRANSFER_COMPLETE && result != ECAT_TRANSFER_PENDING)
        return result;
    f->stream = stream;
    f->active = true;
    f->pending = result == ECAT_TRANSFER_PENDING;
    f->ending = false;
    f->io_error = false;
    f->result = ECAT_TRANSFER_PENDING;
    return result;
}

ecat_transfer_result_t ecat_transfer_file_cancel(ecat_transfer_file_t *f)
{
    if (f == NULL || f->transfer == NULL) return ECAT_TRANSFER_INVALID;
    if (!f->active) return ECAT_TRANSFER_STALE;
    ecat_transfer_result_t result = ecat_transfer_cancel(f->transfer, f, f->token);
    f->active = false;
    f->pending = false;
    f->result = ECAT_TRANSFER_FAILED;
    return result;
}

static ecat_transfer_result_t advance(ecat_transfer_file_t *f, ecat_transfer_result_t result)
{
    f->pending = result == ECAT_TRANSFER_PENDING;
    if (result != ECAT_TRANSFER_COMPLETE && result != ECAT_TRANSFER_PENDING) {
        (void)ecat_transfer_file_cancel(f);
        return ECAT_TRANSFER_FAILED;
    }
    if (result == ECAT_TRANSFER_COMPLETE && f->ending) {
        f->active = false;
        f->result = ECAT_TRANSFER_COMPLETE;
    }
    return f->result;
}

ecat_transfer_result_t ecat_transfer_file_step(ecat_transfer_file_t *f)
{
    if (f == NULL || f->transfer == NULL) return ECAT_TRANSFER_INVALID;
    if (!f->active) return f->result;
    if (f->pending)
        return advance(f, ecat_transfer_poll(f->transfer, f, f->token));

    size_t length = fread(f->buffer, 1U, f->capacity, f->stream);
    if (ferror(f->stream)) {
        f->io_error = true;
        (void)ecat_transfer_file_cancel(f);
        return ECAT_TRANSFER_FAILED;
    }
    if (length != 0U)
        return advance(f, ecat_transfer_write(f->transfer, f, f->token, f->buffer, length));
    if (!feof(f->stream)) {
        f->io_error = true;
        (void)ecat_transfer_file_cancel(f);
        return ECAT_TRANSFER_FAILED;
    }
    f->ending = true;
    return advance(f, ecat_transfer_finish(f->transfer, f, f->token));
}
