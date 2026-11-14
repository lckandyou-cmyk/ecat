#include "ecat_upgrade_file.h"
#include <string.h>

int ecat_upgrade_file_init(ecat_upgrade_file_t *f, ecat_upgrade_t *upgrade,
    uint8_t *buffer, size_t capacity)
{
    if (f == NULL || upgrade == NULL || upgrade->core == NULL || buffer == NULL || capacity == 0U)
        return -1;
    memset(f, 0, sizeof(*f));
    f->upgrade = upgrade;
    f->buffer = buffer;
    f->capacity = capacity;
    f->result = ECAT_UPGRADE_INVALID;
    return 0;
}

ecat_upgrade_result_t ecat_upgrade_file_begin(ecat_upgrade_file_t *f,
    FILE *stream, const ecat_upgrade_request_t *request)
{
    if (f == NULL || f->upgrade == NULL || stream == NULL || request == NULL)
        return ECAT_UPGRADE_INVALID;
    if (f->active) return ECAT_UPGRADE_BUSY;
    ecat_upgrade_result_t result = ecat_upgrade_begin(f->upgrade, f, request, &f->token);
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
    if (f == NULL || f->upgrade == NULL) return ECAT_UPGRADE_INVALID;
    if (!f->active) return ECAT_UPGRADE_STALE;
    ecat_upgrade_result_t result = ecat_upgrade_cancel(f->upgrade, f, f->token);
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
    if (f == NULL || f->upgrade == NULL) return ECAT_UPGRADE_INVALID;
    if (!f->active) return f->result;
    if (f->pending) return advance(f, ecat_upgrade_poll(f->upgrade, f, f->token));
    size_t length = fread(f->buffer, 1U, f->capacity, f->stream);
    if (ferror(f->stream)) {
        f->io_error = true;
        (void)ecat_upgrade_file_cancel(f);
        return ECAT_UPGRADE_FAILED;
    }
    if (length != 0U)
        return advance(f, ecat_upgrade_write(f->upgrade, f, f->token, f->buffer, length));
    if (!feof(f->stream)) {
        f->io_error = true;
        (void)ecat_upgrade_file_cancel(f);
        return ECAT_UPGRADE_FAILED;
    }
    f->ending = true;
    return advance(f, ecat_upgrade_finish(f->upgrade, f, f->token));
}
