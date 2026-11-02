/**
 * @file ecat_foe_def.h
 * @brief EtherCAT FoE 通用固件升级库基础类型与协议常量定义
 *
 * 遵循 ETG.1000.6 标准 FoE 规范及 .efw 文件格式定义。
 */

#ifndef ECAT_FOE_DEF_H
#define ECAT_FOE_DEF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                        标准 EtherCAT FoE 错误码 (ETG.1000.6)               */
/* -------------------------------------------------------------------------- */
#define ECAT_FOE_ERR_OK                 0x0000U
#define ECAT_FOE_ERR_NOT_DEFINED        0x8000U
#define ECAT_FOE_ERR_NOT_FOUND          0x8001U
#define ECAT_FOE_ERR_ACCESS_DENIED      0x8002U
#define ECAT_FOE_ERR_DISK_FULL          0x8003U
#define ECAT_FOE_ERR_ILLEGAL            0x8004U
#define ECAT_FOE_ERR_PACKET_NO          0x8005U
#define ECAT_FOE_ERR_ALREADY_EXISTS     0x8006U
#define ECAT_FOE_ERR_NO_USER            0x8007U
#define ECAT_FOE_ERR_BOOTSTRAP_ONLY     0x8008U
#define ECAT_FOE_ERR_NOT_BOOTSTRAP      0x8009U
#define ECAT_FOE_ERR_NO_RIGHTS          0x800AU
#define ECAT_FOE_ERR_PROG_ERROR         0x800BU

/* -------------------------------------------------------------------------- */
/*                                升级目标 ID                                 */
/* -------------------------------------------------------------------------- */
typedef enum {
    ECAT_FOE_TARGET_NONE   = 0,
    ECAT_FOE_TARGET_APP    = 1, /* 主微控制器应用程序 (MCU Application) */
    ECAT_FOE_TARGET_FPGA   = 2, /* 逻辑固件 (FPGA/CPLD Bitstream) */
    ECAT_FOE_TARGET_EEPROM = 3, /* 从站信息接口/配置数据 (ESC SII EEPROM) */
    ECAT_FOE_TARGET_CUSTOM = 4  /* 用户自定义扩展目标 */
} ecat_foe_target_id_t;

/* -------------------------------------------------------------------------- */
/*                            .efw 固件文件头规范                             */
/* -------------------------------------------------------------------------- */
#define ECAT_EFW_MAGIC              0x31574645U /* "EFW1" in little-endian ('E' | 'F'<<8 | 'W'<<16 | '1'<<24) */
#define ECAT_EFW_HEADER_VERSION     1U
#define ECAT_EFW_NAME_MAX_LEN       32U
#define ECAT_EFW_VER_MAX_LEN        16U

#pragma pack(push, 1)
/**
 * @brief .efw 固件容器二进制文件头 (共 84 字节，固定紧凑布局)
 */
typedef struct {
    uint32_t magic;                              /* 必须为 ECAT_EFW_MAGIC ("EFW1") */
    uint16_t header_version;                     /* 文件头版本，当前固定为 1 */
    uint16_t header_size;                        /* 文件头大小 = sizeof(ecat_efw_header_t) */
    uint32_t project_id;                         /* 硬件/工程唯一 Project ID (如 0x10010001) */
    uint32_t hardware_id;                        /* 兼容硬件版本 ID (如 0x00000001) */
    uint16_t target_id;                          /* 目标 ID (1: APP, 2: FPGA, 3: EEPROM) */
    uint16_t flags;                              /* 扩展标志位 (保留，默认为 0) */
    uint32_t version_raw;                        /* 原始版本号: (Major<<24) | (Minor<<16) | (Patch<<8) | Build */
    char     project_name[ECAT_EFW_NAME_MAX_LEN];/* 工程标识字符串 (以 '\0' 结尾，如 "SERVO_DRV") */
    char     version_str[ECAT_EFW_VER_MAX_LEN];  /* 版本字符串 (以 '\0' 结尾，如 "1.2.3") */
    uint32_t raw_payload_size;                   /* 固件有效负载字节数 (不包含本头部) */
    uint32_t raw_payload_crc;                    /* 固件有效负载的 IEEE 802.3 CRC32 校验和 */
    uint32_t header_crc;                         /* 文件头的 CRC32 校验和 (计算时此字段填 0) */
} ecat_efw_header_t;
#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* ECAT_FOE_DEF_H */
