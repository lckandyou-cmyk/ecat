/**
 * @file ecat_foe_soes.c
 * @brief SOES 适配转接层实现
 */

#include "ecat_foe_soes.h"
#include <string.h>

static uint32_t s_soes_packet_counter = 0;

uint16_t ecat_foe_soes_write(void *self, uint8_t *name, uint32_t name_len, uint32_t password)
{
    (void)self;

    if (!name || name_len == 0) {
        return ECAT_FOE_ERR_ILLEGAL;
    }

    char filename[128];
    if (name_len >= sizeof(filename)) {
        return ECAT_FOE_ERR_ILLEGAL;
    }
    memcpy(filename, name, name_len);
    filename[name_len] = '\0';

    s_soes_packet_counter = 0;

    uint16_t err = ECAT_FOE_ERR_OK;
    if (!ecat_foe_on_wrq(filename, name_len, password, &err)) {
        return err;
    }

    return ECAT_FOE_ERR_OK;
}

uint16_t ecat_foe_soes_write_data(void *self, uint8_t *data, size_t length, uint8_t is_last)
{
    (void)self;

    if (!data || length == 0) {
        return ECAT_FOE_ERR_ILLEGAL;
    }

    s_soes_packet_counter++;

    uint16_t err = ECAT_FOE_ERR_OK;
    if (!ecat_foe_on_data(s_soes_packet_counter, data, (uint32_t)length, is_last != 0, &err)) {
        return err;
    }

    return ECAT_FOE_ERR_OK;
}

uint16_t ecat_foe_soes_read(void *self, uint8_t *name, uint32_t name_len, uint32_t password)
{
    (void)self;

    if (!name || name_len == 0) {
        return ECAT_FOE_ERR_NOT_FOUND;
    }

    char filename[128];
    if (name_len >= sizeof(filename)) {
        return ECAT_FOE_ERR_ILLEGAL;
    }
    memcpy(filename, name, name_len);
    filename[name_len] = '\0';

    uint16_t err = ECAT_FOE_ERR_OK;
    if (!ecat_foe_on_rrq(filename, name_len, password, &err)) {
        return err;
    }

    return ECAT_FOE_ERR_OK;
}
