/**
 * @file ecat_foe_ssc.c
 * @brief Beckhoff SSC 转接层实现
 */

#include "ecat_foe_ssc.h"
#include <string.h>

static uint32_t s_ssc_packet_counter = 0;

uint16_t ecat_foe_ssc_write(const void *p_name, uint16_t name_len, uint32_t password)
{
    if (!p_name || name_len == 0) {
        return ECAT_FOE_ERR_ILLEGAL;
    }

    /* SSC 传入的文件名缓冲区可能不是以 \0 结尾的字符串，拷贝至临时缓冲区 */
    char filename[128];
    if (name_len >= sizeof(filename)) {
        return ECAT_FOE_ERR_ILLEGAL;
    }
    memcpy(filename, p_name, name_len);
    filename[name_len] = '\0';

    s_ssc_packet_counter = 0;

    uint16_t err = ECAT_FOE_ERR_OK;
    if (!ecat_foe_on_wrq(filename, name_len, password, &err)) {
        return err;
    }

    return ECAT_FOE_ERR_OK;
}

uint16_t ecat_foe_ssc_data(const void *p_data, uint16_t size, bool b_last)
{
    if (!p_data || size == 0) {
        return ECAT_FOE_ERR_ILLEGAL;
    }

    s_ssc_packet_counter++;

    uint16_t err = ECAT_FOE_ERR_OK;
    if (!ecat_foe_on_data(s_ssc_packet_counter, (const uint8_t *)p_data, (uint32_t)size, b_last, &err)) {
        return err;
    }

    return ECAT_FOE_ERR_OK;
}

uint16_t ecat_foe_ssc_read(const void *p_name, uint16_t name_len, uint32_t password,
                          uint16_t max_block_size, void *p_data)
{
    (void)max_block_size;
    (void)p_data;

    if (!p_name || name_len == 0) {
        return ECAT_FOE_ERR_NOT_FOUND;
    }

    char filename[128];
    if (name_len >= sizeof(filename)) {
        return ECAT_FOE_ERR_ILLEGAL;
    }
    memcpy(filename, p_name, name_len);
    filename[name_len] = '\0';

    uint16_t err = ECAT_FOE_ERR_OK;
    if (!ecat_foe_on_rrq(filename, name_len, password, &err)) {
        return err;
    }

    return ECAT_FOE_ERR_OK;
}

void ecat_foe_ssc_error(uint32_t error_code)
{
    ecat_foe_abort((uint16_t)error_code);
    s_ssc_packet_counter = 0;
}
