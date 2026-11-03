/**
 * @file ecat_foe_core.c
 * @brief EtherCAT FoE 核心升级状态机实现 (依据 SAD-v0.1 第 1、3、4 节)
 */

#include "ecat_foe_core.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/*                       内部文件名逆向切分辅助函数                           */
/* -------------------------------------------------------------------------- */

static bool parse_version_component(const char *str, size_t len, uint32_t *out_val)
{
    if (len == 0 || len > 3) {
        return false;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (str[i] < '0' || str[i] > '9') {
            return false;
        }
        v = v * 10U + (uint32_t)(str[i] - '0');
    }
    if (v > 255U) {
        return false;
    }
    *out_val = v;
    return true;
}

static bool parse_version_string(const char *ver_str, size_t ver_len, uint32_t *out_ver)
{
    if (ver_len == 0) {
        return false;
    }
    if (ver_str[0] == 'v' || ver_str[0] == 'V') {
        ver_str++;
        ver_len--;
    }
    if (ver_len == 0) {
        return false;
    }

    uint32_t parts[4] = {0, 0, 0, 0};
    size_t part_idx = 0;
    size_t start = 0;

    for (size_t i = 0; i <= ver_len; i++) {
        if (i == ver_len || ver_str[i] == '.') {
            if (part_idx >= 4) {
                return false; /* 超过 4 段 */
            }
            if (!parse_version_component(ver_str + start, i - start, &parts[part_idx])) {
                return false;
            }
            part_idx++;
            start = i + 1;
        }
    }

    *out_ver = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

static bool parse_filename_reverse(
    const char *fn,
    const char *expected_project,
    uint16_t *out_target_id,
    uint32_t *out_fw_ver,
    ecat_foe_core_err_t *out_err)
{
    size_t fn_len = strlen(fn);
    if (fn_len < 8) { /* 最短: P_M_V1.efw */
        *out_err = ECAT_FOE_CORE_ERR_WRQ_INVALID_NAME;
        return false;
    }

    /* 1. 检查并剥离末尾 .efw (不区分大小写) */
    const char *ext = fn + fn_len - 4;
    if ((ext[0] != '.') ||
        (ext[1] != 'e' && ext[1] != 'E') ||
        (ext[2] != 'f' && ext[2] != 'F') ||
        (ext[3] != 'w' && ext[3] != 'W')) {
        *out_err = ECAT_FOE_CORE_ERR_WRQ_INVALID_NAME;
        return false;
    }
    size_t base_len = fn_len - 4;

    /* 2. 从右向左寻找第一个下划线: 分割出版本字符串 */
    size_t last_us = base_len;
    while (last_us > 0 && fn[last_us - 1] != '_') {
        last_us--;
    }
    if (last_us == 0 || last_us == base_len) {
        *out_err = ECAT_FOE_CORE_ERR_WRQ_INVALID_NAME;
        return false;
    }
    last_us--; /* fn[last_us] == '_' */

    const char *ver_part = fn + last_us + 1;
    size_t ver_part_len = base_len - (last_us + 1);
    if (!parse_version_string(ver_part, ver_part_len, out_fw_ver)) {
        *out_err = ECAT_FOE_CORE_ERR_WRQ_INVALID_NAME;
        return false;
    }

    /* 3. 从 last_us 继续向左寻找第二个下划线: 分割出目标字样 */
    size_t second_us = last_us;
    while (second_us > 0 && fn[second_us - 1] != '_') {
        second_us--;
    }
    if (second_us == 0) {
        *out_err = ECAT_FOE_CORE_ERR_WRQ_INVALID_NAME;
        return false;
    }
    second_us--; /* fn[second_us] == '_' */

    const char *target_part = fn + second_us + 1;
    size_t target_part_len = last_us - (second_us + 1);

    uint16_t tid = 0;
    if (target_part_len == 3 && strncmp(target_part, "MCU", 3) == 0) {
        tid = ECAT_FOE_TARGET_MCU;
    } else if (target_part_len == 4 && strncmp(target_part, "FPGA", 4) == 0) {
        tid = ECAT_FOE_TARGET_FPGA;
    } else if (target_part_len == 3 && strncmp(target_part, "SII", 3) == 0) {
        tid = ECAT_FOE_TARGET_SII;
    } else if (target_part_len == 6 && strncmp(target_part, "EEPROM", 6) == 0) {
        tid = ECAT_FOE_TARGET_EEPROM;
    } else {
        *out_err = ECAT_FOE_CORE_ERR_WRQ_TARGET_UNKNOWN;
        return false;
    }
    *out_target_id = tid;

    /* 4. 第二个下划线左侧全部字符串为项目名称 */
    size_t proj_len = second_us;
    if (expected_project != NULL) {
        if (strlen(expected_project) != proj_len ||
            strncmp(fn, expected_project, proj_len) != 0) {
            *out_err = ECAT_FOE_CORE_ERR_WRQ_PROJECT_MISMATCH;
            return false;
        }
    }

    return true;
}

/* -------------------------------------------------------------------------- */
/*                               核心机外部接口                               */
/* -------------------------------------------------------------------------- */

void ecat_foe_core_init(ecat_foe_core_t *core, const ecat_foe_core_cfg_t *cfg)
{
    if (core == NULL) {
        return;
    }
    memset(core, 0, sizeof(*core));
    if (cfg != NULL) {
        core->cfg = *cfg;
    }
    core->state = ECAT_FOE_STATE_IDLE;
    core->last_error = ECAT_FOE_CORE_OK;
}

int ecat_foe_core_on_wrq(ecat_foe_core_t *core, const char *filename)
{
    if (core == NULL || filename == NULL) {
        return ECAT_FOE_ERRCODE_NOTDEFINED;
    }

    /* 新会话自愈规则: 若上一个会话残留未闭环，先调驱动 abort() 并重置 */
    if (core->state != ECAT_FOE_STATE_IDLE) {
        if (core->active_drv != NULL && core->active_drv->abort != NULL) {
            core->active_drv->abort(core->active_user_ctx);
        }
        /* 重置会话上下文，保留全局 cfg */
        ecat_foe_core_cfg_t cfg_bak = core->cfg;
        ecat_foe_core_init(core, &cfg_bak);
    }

    /* 逆向切分解析文件名 */
    uint16_t parsed_tid = 0;
    uint32_t parsed_ver = 0;
    ecat_foe_core_err_t err = ECAT_FOE_CORE_OK;

    if (!parse_filename_reverse(filename, core->cfg.project_name, &parsed_tid, &parsed_ver, &err)) {
        core->last_error = err;
        core->state = ECAT_FOE_STATE_ERROR;
        return ecat_foe_core_err_to_foe(err, true, false);
    }

    /* 校验目标在本地配置中是否使能 */
    if (parsed_tid < 1 || parsed_tid > ECAT_FOE_TARGET_COUNT ||
        !core->cfg.targets[parsed_tid - 1].enabled) {
        core->last_error = ECAT_FOE_CORE_ERR_WRQ_TARGET_DISABLED;
        core->state = ECAT_FOE_STATE_ERROR;
        return ECAT_FOE_ERRCODE_NOTFOUND;
    }

    core->req_target_id = parsed_tid;
    core->req_fw_version = parsed_ver;
    core->state = ECAT_FOE_STATE_WRQ_ACKED;
    core->last_error = ECAT_FOE_CORE_OK;
    return 0;
}

static int core_process_payload_bytes(ecat_foe_core_t *core, const uint8_t *payload_data, size_t payload_len)
{
    const ecat_foe_target_driver_t *drv = core->active_drv;
    void *ctx = core->active_user_ctx;
    uint32_t wunit = core->active_write_unit;

    /* 增量累计流 CRC32 与接收统计 */
    ecat_foe_crc32_update(&core->stream_crc, payload_data, payload_len);
    core->payload_rx_bytes += (uint32_t)payload_len;
    core->total_rx_bytes += (uint64_t)payload_len;

    const uint8_t *p = payload_data;
    size_t remaining = payload_len;

    while (remaining > 0) {
        if (core->residual_len > 0) {
            size_t fill = wunit - core->residual_len;
            if (fill > remaining) {
                fill = remaining;
            }
            memcpy(core->residual_buf + core->residual_len, p, fill);
            core->residual_len += (uint32_t)fill;
            p += fill;
            remaining -= fill;

            if (core->residual_len == wunit) {
                /* 满一个 write_unit，检查按需擦除并写入 */
                uint32_t write_end = core->written_offset + wunit;
                if (write_end > core->erased_watermark) {
                    uint32_t erased_end = 0;
                    if (drv->erase_to(write_end, &erased_end, ctx) != 0) {
                        drv->abort(ctx);
                        core->state = ECAT_FOE_STATE_ERROR;
                        core->last_error = ECAT_FOE_CORE_ERR_DRV_ERASE;
                        return -1;
                    }
                    core->erased_watermark = erased_end;
                }
                if (drv->write(core->written_offset, core->residual_buf, wunit, ctx) != 0) {
                    drv->abort(ctx);
                    core->state = ECAT_FOE_STATE_ERROR;
                    core->last_error = ECAT_FOE_CORE_ERR_DRV_WRITE;
                    return -1;
                }
                core->written_offset += wunit;
                core->residual_len = 0;
            }
        } else {
            if (remaining >= wunit) {
                size_t chunks = remaining / wunit;
                uint32_t bytes_to_write = (uint32_t)(chunks * wunit);
                uint32_t write_end = core->written_offset + bytes_to_write;

                if (write_end > core->erased_watermark) {
                    uint32_t erased_end = 0;
                    if (drv->erase_to(write_end, &erased_end, ctx) != 0) {
                        drv->abort(ctx);
                        core->state = ECAT_FOE_STATE_ERROR;
                        core->last_error = ECAT_FOE_CORE_ERR_DRV_ERASE;
                        return -1;
                    }
                    core->erased_watermark = erased_end;
                }
                if (drv->write(core->written_offset, p, bytes_to_write, ctx) != 0) {
                    drv->abort(ctx);
                    core->state = ECAT_FOE_STATE_ERROR;
                    core->last_error = ECAT_FOE_CORE_ERR_DRV_WRITE;
                    return -1;
                }
                core->written_offset += bytes_to_write;
                p += bytes_to_write;
                remaining -= bytes_to_write;
            } else {
                memcpy(core->residual_buf, p, remaining);
                core->residual_len = (uint32_t)remaining;
                remaining = 0;
            }
        }
    }
    return 0;
}

int ecat_foe_core_on_data(ecat_foe_core_t *core, const uint8_t *data, size_t len)
{
    if (core == NULL) {
        return -1;
    }
    if (len == 0) {
        /* 0 长度包表示正常结束 */
        return ecat_foe_core_on_end(core);
    }
    if (data == NULL) {
        core->last_error = ECAT_FOE_CORE_ERR_INVALID_ARG;
        return -1;
    }

    if (core->state != ECAT_FOE_STATE_WRQ_ACKED && core->state != ECAT_FOE_STATE_DATA) {
        core->last_error = ECAT_FOE_CORE_ERR_INVALID_STATE;
        return -1;
    }

    /* 块粒度多收熔断检测 (头部装配完成已知期望总长后生效) */
    if (core->header_complete) {
        uint64_t expected_total = (uint64_t)core->hdr.header_length + (uint64_t)core->hdr.payload_length;
        if (core->total_rx_bytes + (uint64_t)len > expected_total) {
            core->last_error = ECAT_FOE_CORE_ERR_OVER_RECEIVE;
            if (core->active_drv != NULL && core->active_drv->abort != NULL) {
                core->active_drv->abort(core->active_user_ctx);
            }
            core->state = ECAT_FOE_STATE_ERROR;
            return -1; /* 整块拒绝，不写入底层介质 */
        }
    }

    const uint8_t *p_data = data;
    size_t p_len = len;

    /* 头部装配流程 */
    if (!core->header_complete) {
        /* 初次收到 8 字节以上，立即探测阶段 1 */
        if (core->header_rx_len < 8U) {
            size_t probe_copy = 8U - core->header_rx_len;
            if (probe_copy > p_len) {
                probe_copy = p_len;
            }
            memcpy(core->header_buf + core->header_rx_len, p_data, probe_copy);
            core->header_rx_len += (uint16_t)probe_copy;
            p_data += probe_copy;
            p_len -= probe_copy;

            if (core->header_rx_len < 8U) {
                core->total_rx_bytes += probe_copy;
                return 0; /* 尚不足 8 字节探测，安全返回 */
            }

            uint16_t parsed_hlen = 0;
            ecat_foe_efw_err_t err1 = ecat_foe_efw_parse_stage1(core->header_buf, 8, ECAT_FOE_HEADER_BUF_MAX, &parsed_hlen);
            if (err1 != ECAT_FOE_EFW_OK) {
                core->last_error = ECAT_FOE_CORE_ERR_HDR_PROBE;
                core->state = ECAT_FOE_STATE_ERROR;
                return -1;
            }
            core->header_total_len = parsed_hlen;
        }

        /* 攒齐 header_total_len 字节 */
        if (core->header_rx_len < core->header_total_len) {
            size_t h_needed = (size_t)core->header_total_len - (size_t)core->header_rx_len;
            size_t h_copy = (p_len < h_needed) ? p_len : h_needed;
            memcpy(core->header_buf + core->header_rx_len, p_data, h_copy);
            core->header_rx_len += (uint16_t)h_copy;
            p_data += h_copy;
            p_len -= h_copy;

            if (core->header_rx_len < core->header_total_len) {
                core->total_rx_bytes += h_copy;
                return 0; /* 头部尚未攒齐，返回 0 成功，绝对不碰驱动 */
            }
        }

        /* 头部已完整攒齐，执行阶段 2 (Header CRC32) 与阶段 3 (擦除前全量核验) */
        ecat_foe_efw_err_t err2 = ecat_foe_efw_parse_stage2(core->header_buf, core->header_total_len, &core->hdr);
        if (err2 != ECAT_FOE_EFW_OK) {
            core->last_error = ECAT_FOE_CORE_ERR_HDR_CRC;
            core->state = ECAT_FOE_STATE_ERROR;
            return -1;
        }

        /* 转换为阶段 3 本地配置结构 */
        ecat_foe_efw_config_t efw_cfg;
        memset(&efw_cfg, 0, sizeof(efw_cfg));
        efw_cfg.project_id = core->cfg.project_id;
        efw_cfg.hardware_id = core->cfg.hardware_id;
        efw_cfg.local_hw_ver = core->cfg.local_hw_ver;
        efw_cfg.anti_rollback_enable = core->cfg.anti_rollback_enable;

        for (size_t i = 0; i < ECAT_FOE_TARGET_COUNT; i++) {
            efw_cfg.targets[i].enabled = core->cfg.targets[i].enabled;
            efw_cfg.targets[i].max_payload_size = core->cfg.targets[i].max_payload_size;
            if (core->cfg.anti_rollback_enable) {
                uint32_t floor_ver = 0;
                port_get_version_floor((uint16_t)(i + 1), &floor_ver);
                efw_cfg.targets[i].version_floor = floor_ver;
            }
        }

        ecat_foe_efw_err_t err3 = ecat_foe_efw_parse_stage3(&core->hdr, &efw_cfg);
        if (err3 != ECAT_FOE_EFW_OK) {
            core->last_error = ECAT_FOE_CORE_ERR_HDR_VERIFY;
            core->state = ECAT_FOE_STATE_ERROR;
            return -1;
        }

        /* 双向强交叉一致性比对: target 与 version */
        if (core->hdr.target_id != core->req_target_id ||
            core->hdr.fw_version != core->req_fw_version) {
            core->last_error = ECAT_FOE_CORE_ERR_HDR_CROSS_MISMATCH;
            core->state = ECAT_FOE_STATE_ERROR;
            return -1;
        }

        /* 目标驱动绑定 */
        const ecat_foe_target_desc_t *tdesc = &core->cfg.targets[core->hdr.target_id - 1];
        if (tdesc->driver == NULL) {
            core->last_error = ECAT_FOE_CORE_ERR_DRV_NOT_REGISTERED;
            core->state = ECAT_FOE_STATE_ERROR;
            return -1;
        }
        core->active_drv = tdesc->driver;
        core->active_user_ctx = tdesc->user_ctx;
        core->active_write_unit = (tdesc->write_unit > 0) ? tdesc->write_unit : 1;

        /* 驱动生命周期调用: 1. init -> 2. invalidate */
        if (core->active_drv->init != NULL) {
            if (core->active_drv->init(core->active_user_ctx) != 0) {
                core->last_error = ECAT_FOE_CORE_ERR_DRV_INIT;
                core->state = ECAT_FOE_STATE_ERROR;
                return -1;
            }
        }

        if (core->active_drv->invalidate != NULL) {
            if (core->active_drv->invalidate(core->active_user_ctx) != 0) {
                if (core->active_drv->abort != NULL) {
                    core->active_drv->abort(core->active_user_ctx);
                }
                core->last_error = ECAT_FOE_CORE_ERR_DRV_INVALIDATE;
                core->state = ECAT_FOE_STATE_ERROR;
                return -1;
            }
        }

        /* 头部装配闭环并初始化流式写入状态 */
        core->header_complete = true;
        core->state = ECAT_FOE_STATE_DATA;
        core->total_rx_bytes = (uint64_t)core->header_total_len;
        core->payload_rx_bytes = 0;
        core->erased_watermark = 0;
        core->written_offset = 0;
        core->residual_len = 0;
        ecat_foe_crc32_init(&core->stream_crc);
    }

    /* 处理本块中随头部之后附带的有效负载数据 */
    if (p_len > 0) {
        /* 多收即时熔断检查 */
        uint64_t expected_total = (uint64_t)core->hdr.header_length + (uint64_t)core->hdr.payload_length;
        if (core->total_rx_bytes + (uint64_t)p_len > expected_total) {
            core->last_error = ECAT_FOE_CORE_ERR_OVER_RECEIVE;
            if (core->active_drv != NULL && core->active_drv->abort != NULL) {
                core->active_drv->abort(core->active_user_ctx);
            }
            core->state = ECAT_FOE_STATE_ERROR;
            return -1;
        }

        if (core_process_payload_bytes(core, p_data, p_len) != 0) {
            return -1;
        }
    }

    return 0;
}

int ecat_foe_core_on_end(ecat_foe_core_t *core)
{
    if (core == NULL) {
        return -1;
    }
    if (core->state != ECAT_FOE_STATE_DATA) {
        core->last_error = ECAT_FOE_CORE_ERR_INVALID_STATE;
        return -1;
    }

    const ecat_foe_target_driver_t *drv = core->active_drv;
    void *ctx = core->active_user_ctx;

    /* 1. 总长核对 */
    if (core->payload_rx_bytes != core->hdr.payload_length) {
        if (drv != NULL && drv->abort != NULL) {
            drv->abort(ctx);
        }
        core->state = ECAT_FOE_STATE_ERROR;
        core->last_error = ECAT_FOE_CORE_ERR_UNDER_RECEIVE;
        return -1;
    }

    /* 2. 尾部不足 write_unit 的残留以 0xFF 补齐写入 */
    if (core->residual_len > 0) {
        uint32_t wunit = core->active_write_unit;
        size_t pad = wunit - core->residual_len;
        memset(core->residual_buf + core->residual_len, 0xFF, pad);

        uint32_t write_end = core->written_offset + wunit;
        if (write_end > core->erased_watermark) {
            uint32_t erased_end = 0;
            if (drv->erase_to(write_end, &erased_end, ctx) != 0) {
                drv->abort(ctx);
                core->state = ECAT_FOE_STATE_ERROR;
                core->last_error = ECAT_FOE_CORE_ERR_DRV_ERASE;
                return -1;
            }
            core->erased_watermark = erased_end;
        }
        if (drv->write(core->written_offset, core->residual_buf, wunit, ctx) != 0) {
            drv->abort(ctx);
            core->state = ECAT_FOE_STATE_ERROR;
            core->last_error = ECAT_FOE_CORE_ERR_DRV_WRITE;
            return -1;
        }
        core->written_offset += wunit;
        core->residual_len = 0;
    }

    /* 3. 第一道防线: 流 CRC32 比对 */
    uint32_t final_stream_crc = ecat_foe_crc32_final(&core->stream_crc);
    if (final_stream_crc != core->hdr.payload_crc32) {
        if (drv != NULL && drv->abort != NULL) {
            drv->abort(ctx);
        }
        core->state = ECAT_FOE_STATE_ERROR;
        core->last_error = ECAT_FOE_CORE_ERR_STREAM_CRC_MISMATCH;
        return -1;
    }

    /* 4. 第二道防线: 物理介质底层回读比对 (仅覆盖 payload_length 个有效字节，不含尾部 0xFF 填充) */
    uint32_t read_crc;
    ecat_foe_crc32_init(&read_crc);

    uint8_t read_buf[64];
    uint32_t r_offset = 0;
    uint32_t r_remaining = core->hdr.payload_length;

    while (r_remaining > 0) {
        uint32_t r_chunk = (r_remaining < sizeof(read_buf)) ? r_remaining : (uint32_t)sizeof(read_buf);
        if (drv->read(r_offset, read_buf, r_chunk, ctx) != 0) {
            drv->abort(ctx);
            core->state = ECAT_FOE_STATE_ERROR;
            core->last_error = ECAT_FOE_CORE_ERR_DRV_READ;
            return -1;
        }
        ecat_foe_crc32_update(&read_crc, read_buf, r_chunk);
        r_offset += r_chunk;
        r_remaining -= r_chunk;
    }

    uint32_t final_read_crc = ecat_foe_crc32_final(&read_crc);
    if (final_read_crc != core->hdr.payload_crc32) {
        if (drv != NULL && drv->abort != NULL) {
            drv->abort(ctx);
        }
        core->state = ECAT_FOE_STATE_ERROR;
        core->last_error = ECAT_FOE_CORE_ERR_READBACK_CRC_MISMATCH;
        return -1;
    }

    /* 5. 驱动 finalize() 提交 */
    if (drv != NULL && drv->finalize != NULL) {
        if (drv->finalize(ctx) != 0) {
            if (drv->abort != NULL) {
                drv->abort(ctx);
            }
            core->state = ECAT_FOE_STATE_ERROR;
            core->last_error = ECAT_FOE_CORE_ERR_DRV_FINALIZE;
            return -1;
        }
    }

    /* 6. 防回滚更新: finalize 成功后持久化版本下限 */
    if (core->cfg.anti_rollback_enable) {
        uint32_t old_floor = 0;
        port_get_version_floor(core->hdr.target_id, &old_floor);
        uint32_t new_floor = (old_floor > core->hdr.min_allow_version) ? old_floor : core->hdr.min_allow_version;
        port_set_version_floor(core->hdr.target_id, new_floor);
    }

    core->state = ECAT_FOE_STATE_SUCCESS;
    core->last_error = ECAT_FOE_CORE_OK;
    return 0;
}

void ecat_foe_core_on_abort(ecat_foe_core_t *core)
{
    if (core == NULL) {
        return;
    }
    if (core->active_drv != NULL && core->active_drv->abort != NULL) {
        if (core->state == ECAT_FOE_STATE_DATA || core->state == ECAT_FOE_STATE_WRQ_ACKED) {
            core->active_drv->abort(core->active_user_ctx);
        }
    }
    core->state = ECAT_FOE_STATE_IDLE;
}

ecat_foe_core_err_t ecat_foe_core_get_last_error(const ecat_foe_core_t *core)
{
    return (core != NULL) ? core->last_error : ECAT_FOE_CORE_ERR_INVALID_ARG;
}

uint16_t ecat_foe_core_err_to_foe(ecat_foe_core_err_t err, bool is_wrq_stage, bool soes_mode)
{
    if (err == ECAT_FOE_CORE_OK) {
        return 0;
    }

    if (is_wrq_stage) {
        switch (err) {
        case ECAT_FOE_CORE_ERR_WRQ_TARGET_UNKNOWN:
        case ECAT_FOE_CORE_ERR_WRQ_TARGET_DISABLED:
            return ECAT_FOE_ERRCODE_NOTFOUND;
        case ECAT_FOE_CORE_ERR_WRQ_PROJECT_MISMATCH:
            return ECAT_FOE_ERRCODE_ACCESS;
        case ECAT_FOE_CORE_ERR_WRQ_INVALID_NAME:
        default:
            return ECAT_FOE_ERRCODE_ILLEGAL;
        }
    }

    /* 数据传输与结束阶段 */
    if (soes_mode) {
        return ECAT_FOE_ERRCODE_PROGERROR; /* 0x800B 固定收敛 */
    }

    /* 标准细分映射 */
    switch (err) {
    case ECAT_FOE_CORE_ERR_HDR_CRC:
    case ECAT_FOE_CORE_ERR_STREAM_CRC_MISMATCH:
    case ECAT_FOE_CORE_ERR_READBACK_CRC_MISMATCH:
        return ECAT_FOE_ERRCODE_CHECKSUM;
    case ECAT_FOE_CORE_ERR_PAYLOAD_LIMIT_EXCEEDED:
    case ECAT_FOE_CORE_ERR_OVER_RECEIVE:
        return ECAT_FOE_ERRCODE_DISKFULL;
    case ECAT_FOE_CORE_ERR_HDR_PROBE:
    case ECAT_FOE_CORE_ERR_HDR_VERIFY:
    case ECAT_FOE_CORE_ERR_HDR_CROSS_MISMATCH:
        return ECAT_FOE_ERRCODE_ILLEGAL;
    default:
        return ECAT_FOE_ERRCODE_PROGERROR;
    }
}

const char *ecat_foe_core_err_to_str(ecat_foe_core_err_t err)
{
    switch (err) {
    case ECAT_FOE_CORE_OK: return "OK";
    case ECAT_FOE_CORE_ERR_WRQ_INVALID_NAME: return "Invalid filename format";
    case ECAT_FOE_CORE_ERR_WRQ_PROJECT_MISMATCH: return "Project name mismatch";
    case ECAT_FOE_CORE_ERR_WRQ_TARGET_UNKNOWN: return "Unknown target";
    case ECAT_FOE_CORE_ERR_WRQ_TARGET_DISABLED: return "Target disabled";
    case ECAT_FOE_CORE_ERR_HDR_PROBE: return "Header stage 1 probe failed";
    case ECAT_FOE_CORE_ERR_HDR_CRC: return "Header stage 2 CRC mismatch";
    case ECAT_FOE_CORE_ERR_HDR_VERIFY: return "Header stage 3 verification failed";
    case ECAT_FOE_CORE_ERR_HDR_CROSS_MISMATCH: return "Header vs WRQ cross-check mismatch";
    case ECAT_FOE_CORE_ERR_PAYLOAD_LIMIT_EXCEEDED: return "Payload length exceeds partition capacity";
    case ECAT_FOE_CORE_ERR_DRV_NOT_REGISTERED: return "Driver not registered";
    case ECAT_FOE_CORE_ERR_DRV_INIT: return "Driver init() failed";
    case ECAT_FOE_CORE_ERR_DRV_INVALIDATE: return "Driver invalidate() failed";
    case ECAT_FOE_CORE_ERR_DRV_ERASE: return "Driver erase_to() failed";
    case ECAT_FOE_CORE_ERR_DRV_WRITE: return "Driver write() failed";
    case ECAT_FOE_CORE_ERR_DRV_READ: return "Driver read() failed";
    case ECAT_FOE_CORE_ERR_DRV_FINALIZE: return "Driver finalize() failed";
    case ECAT_FOE_CORE_ERR_OVER_RECEIVE: return "Over-receive detected";
    case ECAT_FOE_CORE_ERR_UNDER_RECEIVE: return "Under-receive detected";
    case ECAT_FOE_CORE_ERR_STREAM_CRC_MISMATCH: return "Stream CRC32 mismatch";
    case ECAT_FOE_CORE_ERR_READBACK_CRC_MISMATCH: return "Readback CRC32 mismatch";
    case ECAT_FOE_CORE_ERR_INVALID_STATE: return "Invalid state";
    case ECAT_FOE_CORE_ERR_INVALID_ARG: return "Invalid argument";
    default: return "Unknown error";
    }
}
