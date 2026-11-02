/**
 * @file ecat_foe_core.c
 * @brief EtherCAT FoE 通用固件升级核心状态机与协议处理实现
 */

#include "ecat_foe.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/*                                内部类型与常量                              */
/* -------------------------------------------------------------------------- */
#define ECAT_MAX_TARGETS                4U
#define CRC32_IEEE_POLYNOMIAL           0xEDB88320U

typedef struct {
    const ecat_foe_target_ops_t *ops;
    void *user_ctx;
} ecat_registered_target_t;

typedef struct {
    ecat_foe_state_t state;
    ecat_foe_config_t config;

    /* 注册的目标驱动表 */
    ecat_registered_target_t targets[ECAT_MAX_TARGETS];
    uint8_t target_count;

    /* 当前活跃升级会话状态 */
    uint16_t active_target_id;
    const ecat_foe_partition_t *active_partition;
    const ecat_registered_target_t *active_driver;

    char wrq_project[ECAT_EFW_NAME_MAX_LEN];
    char wrq_target_str[16];
    char wrq_version[ECAT_EFW_VER_MAX_LEN];

    /* 头部缓存与解析状态 */
    uint8_t header_buffer[sizeof(ecat_efw_header_t)];
    size_t header_bytes_received;
    bool header_validated;
    ecat_efw_header_t parsed_header;

    /* 流式有效负载接收与校验追踪 */
    uint32_t expected_packet_no;
    uint32_t payload_bytes_written;
    uint32_t payload_running_crc;
} ecat_foe_ctx_t;

static ecat_foe_ctx_t s_ctx;

/* -------------------------------------------------------------------------- */
/*                               CRC32 辅助函数                               */
/* -------------------------------------------------------------------------- */
uint32_t ecat_crc32_calc(uint32_t init_crc, const uint8_t *data, size_t len)
{
    uint32_t crc = init_crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint32_t)data[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 1U) {
                crc = (crc >> 1) ^ CRC32_IEEE_POLYNOMIAL;
            } else {
                crc = crc >> 1;
            }
        }
    }
    return crc;
}

static uint32_t ecat_crc32_finish(uint32_t crc)
{
    return ~crc;
}

/* -------------------------------------------------------------------------- */
/*                               内部辅助函数                                 */
/* -------------------------------------------------------------------------- */
static const ecat_foe_partition_t *find_partition(uint16_t target_id)
{
    for (uint8_t i = 0; i < s_ctx.config.partition_count; ++i) {
        if (s_ctx.config.partitions[i].target_id == target_id) {
            return &s_ctx.config.partitions[i];
        }
    }
    return NULL;
}

static const ecat_registered_target_t *find_driver(uint16_t target_id)
{
    for (uint8_t i = 0; i < s_ctx.target_count; ++i) {
        if (s_ctx.targets[i].ops && s_ctx.targets[i].ops->target_id == target_id) {
            return &s_ctx.targets[i];
        }
    }
    return NULL;
}

static uint16_t parse_target_str_to_id(const char *target_str)
{
    if (strcmp(target_str, "APP") == 0) {
        return ECAT_FOE_TARGET_APP;
    }
    if (strcmp(target_str, "FPGA") == 0) {
        return ECAT_FOE_TARGET_FPGA;
    }
    if (strcmp(target_str, "EEPROM") == 0) {
        return ECAT_FOE_TARGET_EEPROM;
    }
    return ECAT_FOE_TARGET_NONE;
}

/**
 * @brief 解析文件名格式：<PROJECT>_<TARGET>_<VERSION>.efw
 */
static bool parse_efw_filename(const char *filename, uint32_t len,
                               char *out_proj, size_t proj_max,
                               char *out_target, size_t target_max,
                               char *out_ver, size_t ver_max)
{
    if (!filename || len < 8) { /* 至少形如 A_B_C.efw */
        return false;
    }

    /* 检查后缀是否为 .efw 或 .EFW */
    if (len < 4) {
        return false;
    }
    const char *ext = &filename[len - 4];
    if (ext[0] != '.' ||
        (ext[1] != 'e' && ext[1] != 'E') ||
        (ext[2] != 'f' && ext[2] != 'F') ||
        (ext[3] != 'w' && ext[3] != 'W')) {
        return false;
    }

    /* 提取无后缀的主文件名 */
    char base_name[128];
    size_t base_len = len - 4;
    if (base_len >= sizeof(base_name)) {
        return false;
    }
    memcpy(base_name, filename, base_len);
    base_name[base_len] = '\0';

    /* 从后往前查找下划线 '_'：格式为 <PROJECT>_<TARGET>_<VERSION>
     * 1. 最后一个下划线后为 version (如 "1.0.0")
     * 2. 倒数第二个下划线与最后一个下划线之间为 target (如 "APP", "FPGA")
     * 3. 倒数第二个下划线之前为 project (可包含下划线，如 "SERVO_DRV")
     */
    char *last_underscore = strrchr(base_name, '_');
    if (!last_underscore) {
        return false;
    }
    *last_underscore = '\0';
    const char *p_ver = last_underscore + 1;

    char *second_last_underscore = strrchr(base_name, '_');
    if (!second_last_underscore) {
        return false;
    }
    *second_last_underscore = '\0';
    const char *p_target = second_last_underscore + 1;
    const char *p_proj = base_name;

    if (strlen(p_proj) == 0 || strlen(p_target) == 0 || strlen(p_ver) == 0) {
        return false;
    }

    if (strlen(p_proj) >= proj_max || strlen(p_target) >= target_max || strlen(p_ver) >= ver_max) {
        return false;
    }

    strcpy(out_proj, p_proj);
    strcpy(out_target, p_target);
    strcpy(out_ver, p_ver);

    return true;
}

/* -------------------------------------------------------------------------- */
/*                               公共 API 实现                                */
/* -------------------------------------------------------------------------- */
int ecat_foe_init(const ecat_foe_config_t *cfg)
{
    if (!cfg) {
        return -1;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.state = ECAT_FOE_STATE_IDLE;
    memcpy(&s_ctx.config, cfg, sizeof(ecat_foe_config_t));

    return 0;
}

int ecat_foe_register_target(const ecat_foe_target_ops_t *ops, void *user_ctx)
{
    if (!ops || ops->target_id == ECAT_FOE_TARGET_NONE) {
        return -1;
    }

    if (s_ctx.target_count >= ECAT_MAX_TARGETS) {
        return -1;
    }

    /* 检查是否已注册 */
    for (uint8_t i = 0; i < s_ctx.target_count; ++i) {
        if (s_ctx.targets[i].ops && s_ctx.targets[i].ops->target_id == ops->target_id) {
            s_ctx.targets[i].ops = ops;
            s_ctx.targets[i].user_ctx = user_ctx;
            return 0;
        }
    }

    s_ctx.targets[s_ctx.target_count].ops = ops;
    s_ctx.targets[s_ctx.target_count].user_ctx = user_ctx;
    s_ctx.target_count++;

    return 0;
}

bool ecat_foe_on_wrq(const char *filename, uint32_t filename_len, uint32_t password, uint16_t *err_code)
{
    (void)password;

    if (!err_code) {
        return false;
    }

    *err_code = ECAT_FOE_ERR_OK;

    if (s_ctx.state != ECAT_FOE_STATE_IDLE && s_ctx.state != ECAT_FOE_STATE_COMPLETED) {
        *err_code = ECAT_FOE_ERR_ILLEGAL;
        return false;
    }

    /* 重置会话上下文 */
    s_ctx.active_target_id = ECAT_FOE_TARGET_NONE;
    s_ctx.active_partition = NULL;
    s_ctx.active_driver = NULL;
    s_ctx.header_bytes_received = 0;
    s_ctx.header_validated = false;
    s_ctx.expected_packet_no = 1;
    s_ctx.payload_bytes_written = 0;
    s_ctx.payload_running_crc = 0xFFFFFFFFU;

    /* 解析文件名规范 */
    if (!parse_efw_filename(filename, filename_len,
                           s_ctx.wrq_project, sizeof(s_ctx.wrq_project),
                           s_ctx.wrq_target_str, sizeof(s_ctx.wrq_target_str),
                           s_ctx.wrq_version, sizeof(s_ctx.wrq_version))) {
        *err_code = ECAT_FOE_ERR_ILLEGAL;
        s_ctx.state = ECAT_FOE_STATE_ERROR;
        return false;
    }

    /* 1. 校验工程名称是否与配置匹配 */
    if (s_ctx.config.project_name && strlen(s_ctx.config.project_name) > 0) {
        if (strcmp(s_ctx.wrq_project, s_ctx.config.project_name) != 0) {
            *err_code = ECAT_FOE_ERR_ACCESS_DENIED;
            s_ctx.state = ECAT_FOE_STATE_ERROR;
            return false;
        }
    }

    /* 2. 校验目标类型是否合法与使能 */
    uint16_t target_id = parse_target_str_to_id(s_ctx.wrq_target_str);
    if (target_id == ECAT_FOE_TARGET_NONE) {
        *err_code = ECAT_FOE_ERR_ILLEGAL;
        s_ctx.state = ECAT_FOE_STATE_ERROR;
        return false;
    }

    const ecat_foe_partition_t *part = find_partition(target_id);
    if (!part || !part->enabled) {
        *err_code = ECAT_FOE_ERR_ACCESS_DENIED;
        s_ctx.state = ECAT_FOE_STATE_ERROR;
        return false;
    }

    const ecat_registered_target_t *drv = find_driver(target_id);
    if (!drv || !drv->ops) {
        *err_code = ECAT_FOE_ERR_NOT_FOUND;
        s_ctx.state = ECAT_FOE_STATE_ERROR;
        return false;
    }

    s_ctx.active_target_id = target_id;
    s_ctx.active_partition = part;
    s_ctx.active_driver = drv;
    s_ctx.state = ECAT_FOE_STATE_WRQ_ACKED;

    return true;
}

static bool validate_efw_header(const ecat_efw_header_t *hdr, uint16_t *err_code)
{
    *err_code = ECAT_FOE_ERR_OK;

    /* 1. Magic 字检查 */
    if (hdr->magic != ECAT_EFW_MAGIC) {
        *err_code = ECAT_FOE_ERR_ILLEGAL;
        return false;
    }

    /* 2. 头部版本与尺寸检查 */
    if (hdr->header_version != ECAT_EFW_HEADER_VERSION || hdr->header_size != sizeof(ecat_efw_header_t)) {
        *err_code = ECAT_FOE_ERR_ILLEGAL;
        return false;
    }

    /* 3. 头部 CRC32 检查 (计算时不含 header_crc 字段本身) */
    ecat_efw_header_t tmp_hdr;
    memcpy(&tmp_hdr, hdr, sizeof(ecat_efw_header_t));
    tmp_hdr.header_crc = 0;
    uint32_t calc_hdr_crc = ecat_crc32_finish(ecat_crc32_calc(0xFFFFFFFFU, (const uint8_t *)&tmp_hdr, sizeof(tmp_hdr)));
    if (calc_hdr_crc != hdr->header_crc) {
        *err_code = ECAT_FOE_ERR_PROG_ERROR;
        return false;
    }

    /* 4. 双重匹配：Project ID 与 Hardware ID 必须吻合 */
    if (s_ctx.config.project_id != 0 && hdr->project_id != s_ctx.config.project_id) {
        *err_code = ECAT_FOE_ERR_ACCESS_DENIED;
        return false;
    }
    if (s_ctx.config.hardware_id != 0 && hdr->hardware_id != s_ctx.config.hardware_id) {
        *err_code = ECAT_FOE_ERR_ACCESS_DENIED;
        return false;
    }

    /* 5. 双重匹配：Target ID 必须与 WRQ 请求的文件名一致 */
    if (hdr->target_id != s_ctx.active_target_id) {
        *err_code = ECAT_FOE_ERR_ILLEGAL;
        return false;
    }

    /* 6. 双重匹配：工程名称字符串与文件名一致 */
    if (strncmp(hdr->project_name, s_ctx.wrq_project, ECAT_EFW_NAME_MAX_LEN) != 0) {
        *err_code = ECAT_FOE_ERR_ILLEGAL;
        return false;
    }

    /* 7. 分区尺寸安全边界检查：绝对不采信文件内地址，严格校验文件大小是否超过本地最大分区限额 */
    if (hdr->raw_payload_size == 0) {
        *err_code = ECAT_FOE_ERR_ILLEGAL;
        return false;
    }
    if (hdr->raw_payload_size > s_ctx.active_partition->max_size) {
        *err_code = ECAT_FOE_ERR_DISK_FULL;
        return false;
    }

    return true;
}

bool ecat_foe_on_data(uint32_t packet_no, const uint8_t *data, uint32_t len, bool is_last, uint16_t *err_code)
{
    if (!err_code) {
        return false;
    }
    *err_code = ECAT_FOE_ERR_OK;

    if (!data || len == 0) {
        *err_code = ECAT_FOE_ERR_ILLEGAL;
        s_ctx.state = ECAT_FOE_STATE_ERROR;
        return false;
    }

    /* 包序号连续性校验 */
    if (packet_no != s_ctx.expected_packet_no) {
        *err_code = ECAT_FOE_ERR_PACKET_NO;
        s_ctx.state = ECAT_FOE_STATE_ERROR;
        return false;
    }
    s_ctx.expected_packet_no++;

    const uint8_t *payload_ptr = data;
    size_t payload_len = len;

    /* 阶段一：尚未完成头部解析 */
    if (!s_ctx.header_validated) {
        size_t needed = sizeof(ecat_efw_header_t) - s_ctx.header_bytes_received;
        size_t take = (payload_len < needed) ? payload_len : needed;

        memcpy(&s_ctx.header_buffer[s_ctx.header_bytes_received], payload_ptr, take);
        s_ctx.header_bytes_received += take;
        payload_ptr += take;
        payload_len -= take;

        if (s_ctx.header_bytes_received == sizeof(ecat_efw_header_t)) {
            memcpy(&s_ctx.parsed_header, s_ctx.header_buffer, sizeof(ecat_efw_header_t));

            /* 严谨校验文件头与双重识别规则 */
            if (!validate_efw_header(&s_ctx.parsed_header, err_code)) {
                s_ctx.state = ECAT_FOE_STATE_ERROR;
                if (s_ctx.active_driver && s_ctx.active_driver->ops && s_ctx.active_driver->ops->abort) {
                    s_ctx.active_driver->ops->abort(s_ctx.active_driver->user_ctx);
                }
                return false;
            }

            s_ctx.header_validated = true;
            s_ctx.state = ECAT_FOE_STATE_HEADER_PARSED;

            /* 触发底层驱动初始化与擦除 */
            if (s_ctx.active_driver->ops->init) {
                if (s_ctx.active_driver->ops->init(s_ctx.active_driver->user_ctx) != 0) {
                    *err_code = ECAT_FOE_ERR_PROG_ERROR;
                    s_ctx.state = ECAT_FOE_STATE_ERROR;
                    return false;
                }
            }

            /* 写入地址只按 Bootloader 本地配置的基地址，擦除对应大小 */
            if (s_ctx.active_driver->ops->erase) {
                if (s_ctx.active_driver->ops->erase(0, s_ctx.parsed_header.raw_payload_size,
                                                   s_ctx.active_driver->user_ctx) != 0) {
                    *err_code = ECAT_FOE_ERR_PROG_ERROR;
                    s_ctx.state = ECAT_FOE_STATE_ERROR;
                    return false;
                }
            }

            s_ctx.state = ECAT_FOE_STATE_RECEIVING;
        }
    }

    /* 阶段二：流式写入有效负载数据 */
    if (s_ctx.header_validated && payload_len > 0) {
        /* 防溢出检查 */
        if (s_ctx.payload_bytes_written + payload_len > s_ctx.parsed_header.raw_payload_size) {
            *err_code = ECAT_FOE_ERR_DISK_FULL;
            s_ctx.state = ECAT_FOE_STATE_ERROR;
            if (s_ctx.active_driver->ops->abort) {
                s_ctx.active_driver->ops->abort(s_ctx.active_driver->user_ctx);
            }
            return false;
        }

        /* 调用驱动写入 */
        if (s_ctx.active_driver->ops->write) {
            int ret = s_ctx.active_driver->ops->write(s_ctx.payload_bytes_written, payload_ptr,
                                                      (uint32_t)payload_len, s_ctx.active_driver->user_ctx);
            if (ret != 0) {
                *err_code = ECAT_FOE_ERR_PROG_ERROR;
                s_ctx.state = ECAT_FOE_STATE_ERROR;
                if (s_ctx.active_driver->ops->abort) {
                    s_ctx.active_driver->ops->abort(s_ctx.active_driver->user_ctx);
                }
                return false;
            }
        }

        /* 增量计算 CRC32 */
        s_ctx.payload_running_crc = ecat_crc32_calc(s_ctx.payload_running_crc, payload_ptr, payload_len);
        s_ctx.payload_bytes_written += (uint32_t)payload_len;
    }

    /* 阶段三：尾包校验与提交 */
    if (is_last) {
        if (!s_ctx.header_validated) {
            *err_code = ECAT_FOE_ERR_ILLEGAL;
            s_ctx.state = ECAT_FOE_STATE_ERROR;
            return false;
        }

        if (s_ctx.payload_bytes_written != s_ctx.parsed_header.raw_payload_size) {
            *err_code = ECAT_FOE_ERR_PROG_ERROR;
            s_ctx.state = ECAT_FOE_STATE_ERROR;
            if (s_ctx.active_driver->ops->abort) {
                s_ctx.active_driver->ops->abort(s_ctx.active_driver->user_ctx);
            }
            return false;
        }

        s_ctx.state = ECAT_FOE_STATE_VERIFYING;

        /* 计算最终 CRC32 并比对 */
        uint32_t final_crc = ecat_crc32_finish(s_ctx.payload_running_crc);
        if (final_crc != s_ctx.parsed_header.raw_payload_crc) {
            *err_code = ECAT_FOE_ERR_PROG_ERROR;
            s_ctx.state = ECAT_FOE_STATE_ERROR;
            if (s_ctx.active_driver->ops->abort) {
                s_ctx.active_driver->ops->abort(s_ctx.active_driver->user_ctx);
            }
            return false;
        }

        /* 调用驱动的可选校验 */
        if (s_ctx.active_driver->ops->verify) {
            if (s_ctx.active_driver->ops->verify(s_ctx.payload_bytes_written, final_crc,
                                                 s_ctx.active_driver->user_ctx) != 0) {
                *err_code = ECAT_FOE_ERR_PROG_ERROR;
                s_ctx.state = ECAT_FOE_STATE_ERROR;
                if (s_ctx.active_driver->ops->abort) {
                    s_ctx.active_driver->ops->abort(s_ctx.active_driver->user_ctx);
                }
                return false;
            }
        }

        /* 驱动最终提交 (标记就绪/切换启动向量) */
        if (s_ctx.active_driver->ops->finalize) {
            if (s_ctx.active_driver->ops->finalize(true, s_ctx.active_driver->user_ctx) != 0) {
                *err_code = ECAT_FOE_ERR_PROG_ERROR;
                s_ctx.state = ECAT_FOE_STATE_ERROR;
                return false;
            }
        }

        s_ctx.state = ECAT_FOE_STATE_COMPLETED;
    }

    return true;
}

bool ecat_foe_on_rrq(const char *filename, uint32_t filename_len, uint32_t password, uint16_t *err_code)
{
    (void)filename;
    (void)filename_len;
    (void)password;

    /* 固件升级库默认不开放固件回读 (安全防逆向)，如有需要可针对特定 diagnostic 文件开放 */
    if (err_code) {
        *err_code = ECAT_FOE_ERR_ACCESS_DENIED;
    }
    return false;
}

void ecat_foe_abort(uint16_t err_code)
{
    (void)err_code;
    if (s_ctx.active_driver && s_ctx.active_driver->ops && s_ctx.active_driver->ops->abort) {
        s_ctx.active_driver->ops->abort(s_ctx.active_driver->user_ctx);
    }
    if (s_ctx.active_driver && s_ctx.active_driver->ops && s_ctx.active_driver->ops->finalize) {
        s_ctx.active_driver->ops->finalize(false, s_ctx.active_driver->user_ctx);
    }
    s_ctx.state = ECAT_FOE_STATE_ERROR;
}

void ecat_foe_reset(void)
{
    s_ctx.state = ECAT_FOE_STATE_IDLE;
    s_ctx.active_target_id = ECAT_FOE_TARGET_NONE;
    s_ctx.active_partition = NULL;
    s_ctx.active_driver = NULL;
    s_ctx.header_bytes_received = 0;
    s_ctx.header_validated = false;
    s_ctx.expected_packet_no = 1;
    s_ctx.payload_bytes_written = 0;
    s_ctx.payload_running_crc = 0xFFFFFFFFU;
}

ecat_foe_state_t ecat_foe_get_state(void)
{
    return s_ctx.state;
}

void ecat_foe_get_progress(uint32_t *written_payload, uint32_t *total_payload)
{
    if (written_payload) {
        *written_payload = s_ctx.payload_bytes_written;
    }
    if (total_payload) {
        *total_payload = s_ctx.header_validated ? s_ctx.parsed_header.raw_payload_size : 0;
    }
}

uint16_t ecat_foe_get_active_target(void)
{
    return s_ctx.active_target_id;
}
