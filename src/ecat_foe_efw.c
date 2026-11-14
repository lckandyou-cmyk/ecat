/**
 * @file ecat_foe_efw.c
 * @brief EtherCAT .efw 固件容器格式解析与核验实现
 */

#include "ecat_foe_efw.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/*                        IEEE 802.3 CRC32 预计算表                           */
/* -------------------------------------------------------------------------- */

static const uint32_t s_crc32_table[256] = {
    0x00000000U, 0x77073096U, 0xEE0E612CU, 0x990951BAU,
    0x076DC419U, 0x706AF48FU, 0xE963A535U, 0x9E6495A3U,
    0x0EDB8832U, 0x79DCB8A4U, 0xE0D5E91EU, 0x97D2D988U,
    0x09B64C2BU, 0x7EB17CBDU, 0xE7B82D07U, 0x90BF1D91U,
    0x1DB71064U, 0x6AB020F2U, 0xF3B97148U, 0x84BE41DEU,
    0x1ADAD47DU, 0x6DDDE4EBU, 0xF4D4B551U, 0x83D385C7U,
    0x136C9856U, 0x646BA8C0U, 0xFD62F97AU, 0x8A65C9ECU,
    0x14015C4FU, 0x63066CD9U, 0xFA0F3D63U, 0x8D080DF5U,
    0x3B6E20C8U, 0x4C69105EU, 0xD56041E4U, 0xA2677172U,
    0x3C03E4D1U, 0x4B04D447U, 0xD20D85FDU, 0xA50AB56BU,
    0x35B5A8FAU, 0x42B2986CU, 0xDBBBC9D6U, 0xACBCF940U,
    0x32D86CE3U, 0x45DF5C75U, 0xDCD60DCFU, 0xABD13D59U,
    0x26D930ACU, 0x51DE003AU, 0xC8D75180U, 0xBFD06116U,
    0x21B4F4B5U, 0x56B3C423U, 0xCFBA9599U, 0xB8BDA50FU,
    0x2802B89EU, 0x5F058808U, 0xC60CD9B2U, 0xB10BE924U,
    0x2F6F7C87U, 0x58684C11U, 0xC1611DABU, 0xB6662D3DU,
    0x76DC4190U, 0x01DB7106U, 0x98D220BCU, 0xEFD5102AU,
    0x71B18589U, 0x06B6B51FU, 0x9FBFE4A5U, 0xE8B8D433U,
    0x7807C9A2U, 0x0F00F934U, 0x9609A88EU, 0xE10E9818U,
    0x7F6A0DBBU, 0x086D3D2DU, 0x91646C97U, 0xE6635C01U,
    0x6B6B51F4U, 0x1C6C6162U, 0x856530D8U, 0xF262004EU,
    0x6C0695EDU, 0x1B01A57BU, 0x8208F4C1U, 0xF50FC457U,
    0x65B0D9C6U, 0x12B7E950U, 0x8BBEB8EAU, 0xFCB9887CU,
    0x62DD1DDFU, 0x15DA2D49U, 0x8CD37CF3U, 0xFBD44C65U,
    0x4DB26158U, 0x3AB551CEU, 0xA3BC0074U, 0xD4BB30E2U,
    0x4ADFA541U, 0x3DD895D7U, 0xA4D1C46DU, 0xD3D6F4FBU,
    0x4369E96AU, 0x346ED9FCU, 0xAD678846U, 0xDA60B8D0U,
    0x44042D73U, 0x33031DE5U, 0xAA0A4C5FU, 0xDD0D7CC9U,
    0x5005713CU, 0x270241AAU, 0xBE0B1010U, 0xC90C2086U,
    0x5768B525U, 0x206F85B3U, 0xB966D409U, 0xCE61E49FU,
    0x5EDEF90EU, 0x29D9C998U, 0xB0D09822U, 0xC7D7A8B4U,
    0x59B33D17U, 0x2EB40D81U, 0xB7BD5C3BU, 0xC0BA6CADU,
    0xEDB88320U, 0x9ABFB3B6U, 0x03B6E20CU, 0x74B1D29AU,
    0xEAD54739U, 0x9DD277AFU, 0x04DB2615U, 0x73DC1683U,
    0xE3630B12U, 0x94643B84U, 0x0D6D6A3EU, 0x7A6A5AA8U,
    0xE40ECF0BU, 0x9309FF9DU, 0x0A00AE27U, 0x7D079EB1U,
    0xF00F9344U, 0x8708A3D2U, 0x1E01F268U, 0x6906C2FEU,
    0xF762575DU, 0x806567CBU, 0x196C3671U, 0x6E6B06E7U,
    0xFED41B76U, 0x89D32BE0U, 0x10DA7A5AU, 0x67DD4ACCU,
    0xF9B9DF6FU, 0x8EBEEFF9U, 0x17B7BE43U, 0x60B08ED5U,
    0xD6D6A3E8U, 0xA1D1937EU, 0x38D8C2C4U, 0x4FDFF252U,
    0xD1BB67F1U, 0xA6BC5767U, 0x3FB506DDU, 0x48B2364BU,
    0xD80D2BDAU, 0xAF0A1B4CU, 0x36034AF6U, 0x41047A60U,
    0xDF60EFC3U, 0xA867DF55U, 0x316E8EEFU, 0x4669BE79U,
    0xCB61B38CU, 0xBC66831AU, 0x256FD2A0U, 0x5268E236U,
    0xCC0C7795U, 0xBB0B4703U, 0x220216B9U, 0x5505262FU,
    0xC5BA3BBEU, 0xB2BD0B28U, 0x2BB45A92U, 0x5CB36A04U,
    0xC2D7FFA7U, 0xB5D0CF31U, 0x2CD99E8BU, 0x5BDEAE1DU,
    0x9B64C2B0U, 0xEC63F226U, 0x756AA39CU, 0x026D930AU,
    0x9C0906A9U, 0xEB0E363FU, 0x72076785U, 0x05005713U,
    0x95BF4A82U, 0xE2B87A14U, 0x7BB12BAEU, 0x0CB61B38U,
    0x92D28E9BU, 0xE5D5BE0DU, 0x7CDCEFB7U, 0x0BDBDF21U,
    0x86D3D2D4U, 0xF1D4E242U, 0x68DDB3F8U, 0x1FDA836EU,
    0x81BE16CDU, 0xF6B9265BU, 0x6FB077E1U, 0x18B74777U,
    0x88085AE6U, 0xFF0F6A70U, 0x66063BCAU, 0x11010B5CU,
    0x8F659EFFU, 0xF862AE69U, 0x616BFFD3U, 0x166CCF45U,
    0xA00AE278U, 0xD70DD2EEU, 0x4E048354U, 0x3903B3C2U,
    0xA7672661U, 0xD06016F7U, 0x4969474DU, 0x3E6E77DBU,
    0xAED16A4AU, 0xD9D65ADCU, 0x40DF0B66U, 0x37D83BF0U,
    0xA9BCAE53U, 0xDEBB9EC5U, 0x47B2CF7FU, 0x30B5FFE9U,
    0xBDBDF21CU, 0xCABAC28AU, 0x53B39330U, 0x24B4A3A6U,
    0xBAD03605U, 0xCDD70693U, 0x54DE5729U, 0x23D967BFU,
    0xB3667A2EU, 0xC4614AB8U, 0x5D681B02U, 0x2A6F2B94U,
    0xB40BBE37U, 0xC30C8EA1U, 0x5A05DF1BU, 0x2D02EF8DU
};

/* -------------------------------------------------------------------------- */
/*                       内部字节序安全读取辅助函数                           */
/* -------------------------------------------------------------------------- */

static inline uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* -------------------------------------------------------------------------- */
/*                               CRC32 接口实现                               */
/* -------------------------------------------------------------------------- */

void ecat_foe_crc32_init(uint32_t *crc)
{
    if (crc != NULL) {
        *crc = 0xFFFFFFFFU;
    }
}

void ecat_foe_crc32_update(uint32_t *crc, const uint8_t *data, size_t len)
{
    if (crc == NULL || data == NULL) {
        return;
    }

    uint32_t c = *crc;
    for (size_t i = 0; i < len; i++) {
        c = s_crc32_table[(c ^ data[i]) & 0xFFU] ^ (c >> 8);
    }
    *crc = c;
}

uint32_t ecat_foe_crc32_final(const uint32_t *crc)
{
    if (crc == NULL) {
        return 0U;
    }
    return (*crc) ^ 0xFFFFFFFFU;
}

uint32_t ecat_foe_crc32_calc(const uint8_t *data, size_t len)
{
    uint32_t crc;
    ecat_foe_crc32_init(&crc);
    ecat_foe_crc32_update(&crc, data, len);
    return ecat_foe_crc32_final(&crc);
}

/* -------------------------------------------------------------------------- */
/*                            三阶段解析接口实现                              */
/* -------------------------------------------------------------------------- */

ecat_foe_efw_err_t ecat_foe_efw_parse_stage1(
    const uint8_t *buf,
    size_t len,
    uint16_t max_header_len,
    uint16_t *out_header_len)
{
    if (buf == NULL || out_header_len == NULL) {
        return ECAT_FOE_EFW_ERR_INVALID_ARG;
    }
    if (len < ECAT_FOE_EFW_STAGE1_PROBE_LEN) {
        return ECAT_FOE_EFW_ERR_BUFFER_TOO_SMALL;
    }

    uint32_t magic = read_u32_le(buf + 0x00);
    if (magic != ECAT_FOE_EFW_MAGIC) {
        return ECAT_FOE_EFW_ERR_MAGIC;
    }

    uint16_t format_version = read_u16_le(buf + 0x04);
    if (format_version != ECAT_FOE_EFW_FORMAT_VERSION) {
        return ECAT_FOE_EFW_ERR_FORMAT_VERSION;
    }

    uint16_t header_length = read_u16_le(buf + 0x06);
    uint16_t expected_length = ECAT_FOE_EFW_HEADER_MIN_LEN;
    if (header_length < expected_length) return ECAT_FOE_EFW_ERR_HEADER_LEN_MIN;
    if (max_header_len > 0U && header_length > max_header_len) {
        return ECAT_FOE_EFW_ERR_HEADER_LEN_MAX;
    }
    if ((header_length % 4U) != 0U) {
        return ECAT_FOE_EFW_ERR_HEADER_LEN_ALIGN;
    }
    if (header_length != expected_length) return ECAT_FOE_EFW_ERR_SIGNATURE_FORMAT;
    *out_header_len = header_length;
    return ECAT_FOE_EFW_OK;
}

ecat_foe_efw_err_t ecat_foe_efw_parse_stage2(
    const uint8_t *buf,
    size_t len,
    ecat_foe_efw_header_t *out_hdr)
{
    if (buf == NULL || out_hdr == NULL) {
        return ECAT_FOE_EFW_ERR_INVALID_ARG;
    }
    if (len < 8U) {
        return ECAT_FOE_EFW_ERR_BUFFER_TOO_SMALL;
    }

    uint16_t format_version = read_u16_le(buf + 0x04);
    uint16_t header_length = read_u16_le(buf + 0x06);
    uint16_t expected_length = ECAT_FOE_EFW_HEADER_MIN_LEN;
    if ((format_version != ECAT_FOE_EFW_FORMAT_VERSION) ||
        header_length != expected_length || (header_length % 4U) != 0U) {
        return ECAT_FOE_EFW_ERR_HEADER_LEN_MIN;
    }
    if (len < (size_t)expected_length) return ECAT_FOE_EFW_ERR_BUFFER_TOO_SMALL;
    if (len < (size_t)header_length) {
        return ECAT_FOE_EFW_ERR_BUFFER_TOO_SMALL;
    }

    /* 校验 Header CRC32 (覆盖 0 到 header_length - 4 的全部字节) */
    size_t crc_calc_len = (size_t)header_length - 4U;
    uint32_t expected_crc = read_u32_le(buf + crc_calc_len);
    uint32_t actual_crc = ecat_foe_crc32_calc(buf, crc_calc_len);
    if (actual_crc != expected_crc) {
        return ECAT_FOE_EFW_ERR_HEADER_CRC;
    }

    /* 逐字节小端序安全反序列化至输出结构体 */
    out_hdr->magic = read_u32_le(buf + 0x00);
    out_hdr->format_version = read_u16_le(buf + 0x04);
    out_hdr->header_length = header_length;
    out_hdr->project_id = read_u32_le(buf + 0x08);
    out_hdr->hardware_id = read_u32_le(buf + 0x0C);
    out_hdr->hw_ver_min = read_u16_le(buf + 0x10);
    out_hdr->hw_ver_max = read_u16_le(buf + 0x12);
    out_hdr->target_id = read_u16_le(buf + 0x14);
    out_hdr->flags = read_u16_le(buf + 0x16);
    out_hdr->fw_version = read_u32_le(buf + 0x18);
    out_hdr->min_allow_version = read_u32_le(buf + 0x1C);
    out_hdr->payload_length = read_u32_le(buf + 0x20);
    out_hdr->payload_crc32 = read_u32_le(buf + 0x24);

    for (size_t i = 0; i < ECAT_FOE_EFW_RESERVED_LEN; i++) {
        out_hdr->reserved[i] = buf[0x28 + i];
    }
    out_hdr->header_crc32 = expected_crc;
    out_hdr->signed_package = false;
    memset(out_hdr->key_id, 0, sizeof(out_hdr->key_id));
    memset(out_hdr->signature, 0, sizeof(out_hdr->signature));
    static const uint8_t sig_magic[4] = {'S', 'I', 'G', '2'};
    if (memcmp(buf + 60U, sig_magic, sizeof(sig_magic)) != 0 ||
        read_u16_le(buf + 64U) != 1U ||
        (read_u16_le(buf + 66U) != 0U && read_u16_le(buf + 66U) != 1U)) {
        return ECAT_FOE_EFW_ERR_SIGNATURE_FORMAT;
    }
    uint16_t algorithm = read_u16_le(buf + 66U);
    uint16_t signature_len = read_u16_le(buf + 68U);
    uint16_t key_id_len = read_u16_le(buf + 70U);
    if ((algorithm == 0U && (signature_len != 0U || key_id_len != 0U)) ||
        (algorithm == 1U && (signature_len != ECAT_FOE_EFW_SIGNATURE_LEN ||
                             key_id_len != ECAT_FOE_EFW_KEY_ID_LEN))) {
        return ECAT_FOE_EFW_ERR_SIGNATURE_FORMAT;
    }
    if (algorithm == 0U) {
        for (size_t i = 72U; i < 144U; i++) {
            if (buf[i] != 0U) return ECAT_FOE_EFW_ERR_SIGNATURE_FORMAT;
        }
    } else {
        memcpy(out_hdr->key_id, buf + 72U, ECAT_FOE_EFW_KEY_ID_LEN);
        memcpy(out_hdr->signature, buf + 80U, ECAT_FOE_EFW_SIGNATURE_LEN);
        out_hdr->signed_package = true;
    }

    return ECAT_FOE_EFW_OK;
}

ecat_foe_efw_err_t ecat_foe_efw_parse_stage3(
    const ecat_foe_efw_header_t *hdr,
    const ecat_foe_efw_config_t *cfg)
{
    if (hdr == NULL || cfg == NULL) {
        return ECAT_FOE_EFW_ERR_INVALID_ARG;
    }

    /* 1. magic 严格等于 0x57464345 */
    if (hdr->magic != ECAT_FOE_EFW_MAGIC) {
        return ECAT_FOE_EFW_ERR_MAGIC;
    }

    /* Only the current unified container format is accepted. */
    if (hdr->format_version != ECAT_FOE_EFW_FORMAT_VERSION) {
        return ECAT_FOE_EFW_ERR_FORMAT_VERSION;
    }

    if (cfg->signature_verify_enable) {
        if (!hdr->signed_package) {
            return ECAT_FOE_EFW_ERR_SIGNATURE_REQUIRED;
        }
    }

    uint16_t expected_header_length =
        ECAT_FOE_EFW_HEADER_MIN_LEN;
    if (hdr->header_length != expected_header_length || (hdr->header_length % 4U) != 0U) {
        return ECAT_FOE_EFW_ERR_HEADER_LEN_ALIGN;
    }

    /* 4. flags 严格等于 0 */
    if (hdr->flags != 0U) {
        return ECAT_FOE_EFW_ERR_FLAGS_NONZERO;
    }

    /* Reserved bytes 40..59 must remain zero. */
    for (size_t i = 0; i < ECAT_FOE_EFW_RESERVED_LEN; i++) {
        if (hdr->reserved[i] != 0U) {
            return ECAT_FOE_EFW_ERR_RESERVED_NONZERO;
        }
    }

    /* 6. project_id 与本地硬编码配置完全一致 */
    if (hdr->project_id != cfg->project_id) {
        return ECAT_FOE_EFW_ERR_PROJECT_ID_MISMATCH;
    }

    /* 7. hardware_id 与本地硬编码配置完全一致 */
    if (hdr->hardware_id != cfg->hardware_id) {
        return ECAT_FOE_EFW_ERR_HARDWARE_ID_MISMATCH;
    }

    /* 8. hw_ver_min <= hw_ver_max 且本地硬件版本必须落在 [hw_ver_min, hw_ver_max] 内 */
    if (hdr->hw_ver_min > hdr->hw_ver_max) {
        return ECAT_FOE_EFW_ERR_HW_VER_RANGE_INVALID;
    }
    if (cfg->local_hw_ver < hdr->hw_ver_min || cfg->local_hw_ver > hdr->hw_ver_max) {
        return ECAT_FOE_EFW_ERR_HW_VER_NOT_SUPPORTED;
    }

    /* 9. target_id 为已知合法目标 (1..4) 且在本地使能宏中已开启 */
    if (hdr->target_id < 1U || hdr->target_id > ECAT_FOE_TARGET_COUNT) {
        return ECAT_FOE_EFW_ERR_TARGET_UNKNOWN;
    }
    const ecat_foe_efw_target_cfg_t *tcfg = &cfg->targets[hdr->target_id - 1U];
    if (!tcfg->enabled) {
        return ECAT_FOE_EFW_ERR_TARGET_NOT_ENABLED;
    }

    /* 空载荷不能代表可启动固件；在任何目标驱动副作用前拒绝。 */
    if (hdr->payload_length == 0U) {
        return ECAT_FOE_EFW_ERR_PAYLOAD_EMPTY;
    }

    /* 10. payload_length 未超过本地硬编码配置中分配的分区容量上限 */
    if (tcfg->max_payload_size > 0U && hdr->payload_length > tcfg->max_payload_size) {
        return ECAT_FOE_EFW_ERR_PAYLOAD_TOO_LARGE;
    }

    /* 11. min_allow_version <= fw_version */
    if (hdr->min_allow_version > hdr->fw_version) {
        return ECAT_FOE_EFW_ERR_MIN_ALLOW_VER_INVALID;
    }

    /* 12. 若开启防回滚开关，还必须满足 fw_version >= version_floor */
    if (cfg->anti_rollback_enable) {
        if (hdr->fw_version < tcfg->version_floor) {
            return ECAT_FOE_EFW_ERR_ANTI_ROLLBACK;
        }
    }

    return ECAT_FOE_EFW_OK;
}

const char *ecat_foe_efw_err_to_str(ecat_foe_efw_err_t err)
{
    switch (err) {
    case ECAT_FOE_EFW_OK:
        return "OK";
    case ECAT_FOE_EFW_ERR_INVALID_ARG:
        return "Invalid argument";
    case ECAT_FOE_EFW_ERR_BUFFER_TOO_SMALL:
        return "Buffer too small";
    case ECAT_FOE_EFW_ERR_MAGIC:
        return "Magic mismatch";
    case ECAT_FOE_EFW_ERR_FORMAT_VERSION:
        return "Format version unsupported";
    case ECAT_FOE_EFW_ERR_HEADER_LEN_MIN:
        return "Header length is below the supported format minimum";
    case ECAT_FOE_EFW_ERR_HEADER_LEN_MAX:
        return "Header length exceeds local max";
    case ECAT_FOE_EFW_ERR_HEADER_LEN_ALIGN:
        return "Header length not multiple of 4";
    case ECAT_FOE_EFW_ERR_HEADER_CRC:
        return "Header CRC32 mismatch";
    case ECAT_FOE_EFW_ERR_FLAGS_NONZERO:
        return "Flags non-zero";
    case ECAT_FOE_EFW_ERR_RESERVED_NONZERO:
        return "Reserved area non-zero";
    case ECAT_FOE_EFW_ERR_PROJECT_ID_MISMATCH:
        return "Project ID mismatch";
    case ECAT_FOE_EFW_ERR_HARDWARE_ID_MISMATCH:
        return "Hardware ID mismatch";
    case ECAT_FOE_EFW_ERR_HW_VER_RANGE_INVALID:
        return "Hardware version range inverted";
    case ECAT_FOE_EFW_ERR_HW_VER_NOT_SUPPORTED:
        return "Local hardware version not supported";
    case ECAT_FOE_EFW_ERR_TARGET_UNKNOWN:
        return "Target unknown";
    case ECAT_FOE_EFW_ERR_TARGET_NOT_ENABLED:
        return "Target not enabled";
    case ECAT_FOE_EFW_ERR_PAYLOAD_TOO_LARGE:
        return "Payload length exceeds partition capacity";
    case ECAT_FOE_EFW_ERR_MIN_ALLOW_VER_INVALID:
        return "min_allow_version > fw_version";
    case ECAT_FOE_EFW_ERR_ANTI_ROLLBACK:
        return "Anti-rollback check failed";
    case ECAT_FOE_EFW_ERR_PAYLOAD_EMPTY:
        return "Payload length is zero";
    case ECAT_FOE_EFW_ERR_SIGNATURE_FORMAT:
        return "Signature extension format or algorithm unsupported";
    case ECAT_FOE_EFW_ERR_SIGNATURE_REQUIRED:
        return "Signed firmware required";
    default:
        return "Unknown error";
    }
}
