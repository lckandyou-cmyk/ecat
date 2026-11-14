/**
 * @file bootloader_master_flow.c
 * @brief EtherCAT FoE 主站发送交互流程与宿主测试参考实现
 */

#include "bootloader_master_flow.h"
#include "ecat_foe_efw.h"
#include <string.h>

#define MBX_HDR_SIZE 6
#define FOE_HDR_SIZE 6

#define FOE_OP_RRQ   0x01
#define FOE_OP_WRQ   0x02
#define FOE_OP_DATA  0x03
#define FOE_OP_ACK   0x04
#define FOE_OP_ERR   0x05

#define MBX_TYPE_FOE 0x04

#ifndef MBXstate_idle
#define MBXstate_idle    0x00
#define MBXstate_inclaim 0x01
#define MBXstate_outreq  0x02
#endif

/* 真实 SOES 发送函数 */
extern void ESC_writembx(uint8_t n);

size_t bootloader_master_build_efw(uint8_t *out_buf,
                                   size_t out_max,
                                   uint32_t project_id,
                                   const char *project_name,
                                   uint32_t hardware_id,
                                   uint16_t hw_ver_min,
                                   uint16_t hw_ver_max,
                                   uint16_t target_id,
                                   uint32_t fw_ver,
                                   const uint8_t *payload,
                                   size_t payload_len)
{
    (void)project_name;
    if (payload_len > UINT32_MAX || payload_len > SIZE_MAX - ECAT_FOE_EFW_HEADER_MIN_LEN ||
        (payload_len > 0U && payload == NULL)) {
        return 0;
    }
    size_t total_len = ECAT_FOE_EFW_HEADER_MIN_LEN + payload_len;
    if (out_buf == NULL || out_max < total_len) {
        return 0;
    }

    memset(out_buf, 0, ECAT_FOE_EFW_HEADER_MIN_LEN);
    /* 0x00: magic "ECFW" (0x57464345) */
    out_buf[0] = 0x45; out_buf[1] = 0x43; out_buf[2] = 0x46; out_buf[3] = 0x57;
    /* 0x04: format_version = 2 */
    out_buf[4] = 0x02; out_buf[5] = 0x00;
    /* 0x06: header_length = 148 */
    out_buf[6] = 0x94; out_buf[7] = 0x00;
    /* 0x08: project_id (小端序) */
    out_buf[8]  = (uint8_t)(project_id & 0xFF);
    out_buf[9]  = (uint8_t)((project_id >> 8) & 0xFF);
    out_buf[10] = (uint8_t)((project_id >> 16) & 0xFF);
    out_buf[11] = (uint8_t)((project_id >> 24) & 0xFF);
    /* 0x0C: hardware_id (小端序) */
    out_buf[12] = (uint8_t)(hardware_id & 0xFF);
    out_buf[13] = (uint8_t)((hardware_id >> 8) & 0xFF);
    out_buf[14] = (uint8_t)((hardware_id >> 16) & 0xFF);
    out_buf[15] = (uint8_t)((hardware_id >> 24) & 0xFF);
    /* 0x10: hw_ver_min */
    out_buf[16] = (uint8_t)(hw_ver_min & 0xFF);
    out_buf[17] = (uint8_t)((hw_ver_min >> 8) & 0xFF);
    /* 0x12: hw_ver_max */
    out_buf[18] = (uint8_t)(hw_ver_max & 0xFF);
    out_buf[19] = (uint8_t)((hw_ver_max >> 8) & 0xFF);
    /* 0x14: target_id */
    out_buf[20] = (uint8_t)(target_id & 0xFF);
    out_buf[21] = (uint8_t)((target_id >> 8) & 0xFF);
    /* 0x16: flags = 0 */
    out_buf[22] = 0x00; out_buf[23] = 0x00;
    /* 0x18: fw_version */
    out_buf[24] = (uint8_t)(fw_ver & 0xFF);
    out_buf[25] = (uint8_t)((fw_ver >> 8) & 0xFF);
    out_buf[26] = (uint8_t)((fw_ver >> 16) & 0xFF);
    out_buf[27] = (uint8_t)((fw_ver >> 24) & 0xFF);
    /* 0x1C: min_allow_version = 0 */
    out_buf[28] = 0x00; out_buf[29] = 0x00; out_buf[30] = 0x00; out_buf[31] = 0x00;
    /* 0x20: payload_length */
    out_buf[32] = (uint8_t)(payload_len & 0xFF);
    out_buf[33] = (uint8_t)((payload_len >> 8) & 0xFF);
    out_buf[34] = (uint8_t)((payload_len >> 16) & 0xFF);
    out_buf[35] = (uint8_t)((payload_len >> 24) & 0xFF);

    /* 0x24: payload_crc32 */
    uint32_t p_crc = (payload != NULL && payload_len > 0) ? ecat_foe_crc32_calc(payload, payload_len) : 0;
    out_buf[36] = (uint8_t)(p_crc & 0xFF);
    out_buf[37] = (uint8_t)((p_crc >> 8) & 0xFF);
    out_buf[38] = (uint8_t)((p_crc >> 16) & 0xFF);
    out_buf[39] = (uint8_t)((p_crc >> 24) & 0xFF);

    /* 0x28 - 0x3B: 20 字节保留区 (置 0) */
    memset(&out_buf[40], 0x00, 20);

    /* SIG2 unsigned extension; all optional signature fields remain zero. */
    memcpy(out_buf + 60, "SIG2", 4);
    out_buf[64] = 1U;

    /* Header CRC32 covers bytes [0, 144), including the complete extension. */
    uint32_t h_crc = ecat_foe_crc32_calc(out_buf, 144U);
    out_buf[144] = (uint8_t)(h_crc & 0xFF);
    out_buf[145] = (uint8_t)((h_crc >> 8) & 0xFF);
    out_buf[146] = (uint8_t)((h_crc >> 16) & 0xFF);
    out_buf[147] = (uint8_t)((h_crc >> 24) & 0xFF);

    /* 拷贝载荷 */
    if (payload != NULL && payload_len > 0) {
        memcpy(&out_buf[ECAT_FOE_EFW_HEADER_MIN_LEN], payload, payload_len);
    }
    return total_len;
}

/* 内部辅助：检索输出槽位并读取从站响应 */
static int read_and_consume_response(bootloader_app_t *app, master_foe_response_t *resp_out)
{
    uint8_t *mbx = app->adapter.mbx_ctx.mbx_buffers;
    uint16_t slot_size = app->adapter.mbx_ctx.buffer_size;
    uint8_t num_slots = app->adapter.mbx_ctx.num_buffers;
    uint8_t *ctrl_bytes = (uint8_t *)app->adapter.mbx_ctx.mbx_control_states;

    if (mbx == NULL || ctrl_bytes == NULL || num_slots == 0 || slot_size == 0) {
        return -1;
    }

    uint8_t out_slot = 0;
    for (uint8_t i = 1; i < num_slots; i++) {
        if (ctrl_bytes[i] == MBXstate_outreq) {
            out_slot = i;
            break;
        }
    }
    if (out_slot == 0) {
        return -1; /* 未产生输出响应 */
    }

    uint8_t *res_buf = &mbx[out_slot * slot_size];
    if (resp_out != NULL) {
        resp_out->opcode = res_buf[MBX_HDR_SIZE + 0];
        resp_out->mbxcnt = (uint8_t)(res_buf[5] >> 4);
        if (resp_out->opcode == FOE_OP_ACK) {
            resp_out->packet_number = (uint32_t)res_buf[MBX_HDR_SIZE + 2] |
                                     ((uint32_t)res_buf[MBX_HDR_SIZE + 3] << 8) |
                                     ((uint32_t)res_buf[MBX_HDR_SIZE + 4] << 16) |
                                     ((uint32_t)res_buf[MBX_HDR_SIZE + 5] << 24);
            resp_out->error_code = 0;
        } else if (resp_out->opcode == FOE_OP_ERR) {
            resp_out->packet_number = 0;
            resp_out->error_code = (uint32_t)res_buf[MBX_HDR_SIZE + 2] |
                                  ((uint32_t)res_buf[MBX_HDR_SIZE + 3] << 8) |
                                  ((uint32_t)res_buf[MBX_HDR_SIZE + 4] << 16) |
                                  ((uint32_t)res_buf[MBX_HDR_SIZE + 5] << 24);
        } else {
            resp_out->packet_number = 0;
            resp_out->error_code = 0;
        }
    }

    /* 模拟主站消费物理邮箱，释放槽位 */
    ESC_writembx(out_slot);
    ctrl_bytes[out_slot] = MBXstate_idle;
    return 0;
}

int bootloader_master_send_wrq(bootloader_app_t *app,
                               foe_cfg_t *foe_cfg,
                               const char *filename,
                               uint32_t password,
                               master_foe_response_t *resp_out)
{
    if (app == NULL || foe_cfg == NULL || filename == NULL) {
        return -1;
    }
    size_t fn_len = strlen(filename);
    uint8_t *mbx = app->adapter.mbx_ctx.mbx_buffers;
    uint8_t *ctrl_bytes = (uint8_t *)app->adapter.mbx_ctx.mbx_control_states;

    if (mbx == NULL || ctrl_bytes == NULL ||
        app->adapter.mbx_ctx.buffer_size < MBX_HDR_SIZE + FOE_HDR_SIZE ||
        fn_len > (size_t)app->adapter.mbx_ctx.buffer_size - MBX_HDR_SIZE - FOE_HDR_SIZE ||
        fn_len > UINT16_MAX - FOE_HDR_SIZE) {
        return -1;
    }

    /* 格式化槽位 0 (主站接收邮箱) 报文 */
    uint16_t foe_payload_len = (uint16_t)(FOE_HDR_SIZE + fn_len);
    mbx[0] = (uint8_t)(foe_payload_len & 0xFF);
    mbx[1] = (uint8_t)((foe_payload_len >> 8) & 0xFF);
    mbx[2] = 0x00;
    mbx[3] = 0x00;
    mbx[4] = 0x00;
    mbx[5] = MBX_TYPE_FOE;

    mbx[MBX_HDR_SIZE + 0] = FOE_OP_WRQ;
    mbx[MBX_HDR_SIZE + 1] = 0x00; /* reserved */
    mbx[MBX_HDR_SIZE + 2] = (uint8_t)(password & 0xFF);
    mbx[MBX_HDR_SIZE + 3] = (uint8_t)((password >> 8) & 0xFF);
    mbx[MBX_HDR_SIZE + 4] = (uint8_t)((password >> 16) & 0xFF);
    mbx[MBX_HDR_SIZE + 5] = (uint8_t)((password >> 24) & 0xFF);
    memcpy(&mbx[MBX_HDR_SIZE + FOE_HDR_SIZE], filename, fn_len);

    ctrl_bytes[0] = MBXstate_inclaim;

    /* 执行从站轮询流 */
    size_t total_rx_len = MBX_HDR_SIZE + foe_payload_len;
    (void)bootloader_app_step(app, foe_cfg, mbx, total_rx_len, NULL, 0, NULL);

    return read_and_consume_response(app, resp_out);
}

int bootloader_master_send_data(bootloader_app_t *app,
                                foe_cfg_t *foe_cfg,
                                uint32_t packet_number,
                                const uint8_t *data,
                                size_t data_len,
                                master_foe_response_t *resp_out)
{
    if (app == NULL || foe_cfg == NULL || (data_len > 0 && data == NULL)) {
        return -1;
    }
    uint8_t *mbx = app->adapter.mbx_ctx.mbx_buffers;
    uint8_t *ctrl_bytes = (uint8_t *)app->adapter.mbx_ctx.mbx_control_states;

    if (mbx == NULL || ctrl_bytes == NULL ||
        app->adapter.mbx_ctx.buffer_size < MBX_HDR_SIZE + FOE_HDR_SIZE ||
        data_len > (size_t)app->adapter.mbx_ctx.buffer_size - MBX_HDR_SIZE - FOE_HDR_SIZE ||
        data_len > UINT16_MAX - FOE_HDR_SIZE) {
        return -1;
    }

    uint16_t foe_payload_len = (uint16_t)(FOE_HDR_SIZE + data_len);
    mbx[0] = (uint8_t)(foe_payload_len & 0xFF);
    mbx[1] = (uint8_t)((foe_payload_len >> 8) & 0xFF);
    mbx[2] = 0x00;
    mbx[3] = 0x00;
    mbx[4] = 0x00;
    mbx[5] = MBX_TYPE_FOE;

    mbx[MBX_HDR_SIZE + 0] = FOE_OP_DATA;
    mbx[MBX_HDR_SIZE + 1] = 0x00;
    mbx[MBX_HDR_SIZE + 2] = (uint8_t)(packet_number & 0xFF);
    mbx[MBX_HDR_SIZE + 3] = (uint8_t)((packet_number >> 8) & 0xFF);
    mbx[MBX_HDR_SIZE + 4] = (uint8_t)((packet_number >> 16) & 0xFF);
    mbx[MBX_HDR_SIZE + 5] = (uint8_t)((packet_number >> 24) & 0xFF);

    if (data != NULL && data_len > 0) {
        memcpy(&mbx[MBX_HDR_SIZE + FOE_HDR_SIZE], data, data_len);
    }

    ctrl_bytes[0] = MBXstate_inclaim;

    size_t total_rx_len = MBX_HDR_SIZE + foe_payload_len;
    (void)bootloader_app_step(app, foe_cfg, mbx, total_rx_len, NULL, 0, NULL);

    return read_and_consume_response(app, resp_out);
}

int bootloader_master_send_err(bootloader_app_t *app,
                               foe_cfg_t *foe_cfg,
                               uint32_t error_code)
{
    if (app == NULL || foe_cfg == NULL) {
        return -1;
    }
    uint8_t *mbx = app->adapter.mbx_ctx.mbx_buffers;
    uint8_t *ctrl_bytes = (uint8_t *)app->adapter.mbx_ctx.mbx_control_states;

    if (mbx == NULL || ctrl_bytes == NULL) {
        return -1;
    }

    uint16_t foe_payload_len = (uint16_t)FOE_HDR_SIZE;
    mbx[0] = (uint8_t)(foe_payload_len & 0xFF);
    mbx[1] = (uint8_t)((foe_payload_len >> 8) & 0xFF);
    mbx[2] = 0x00;
    mbx[3] = 0x00;
    mbx[4] = 0x00;
    mbx[5] = MBX_TYPE_FOE;

    mbx[MBX_HDR_SIZE + 0] = FOE_OP_ERR;
    mbx[MBX_HDR_SIZE + 1] = 0x00;
    mbx[MBX_HDR_SIZE + 2] = (uint8_t)(error_code & 0xFF);
    mbx[MBX_HDR_SIZE + 3] = (uint8_t)((error_code >> 8) & 0xFF);
    mbx[MBX_HDR_SIZE + 4] = (uint8_t)((error_code >> 16) & 0xFF);
    mbx[MBX_HDR_SIZE + 5] = (uint8_t)((error_code >> 24) & 0xFF);

    ctrl_bytes[0] = MBXstate_inclaim;

    size_t total_rx_len = MBX_HDR_SIZE + foe_payload_len;
    (void)bootloader_app_step(app, foe_cfg, mbx, total_rx_len, NULL, 0, NULL);

    master_foe_response_t dummy;
    (void)read_and_consume_response(app, &dummy);
    return 0;
}
