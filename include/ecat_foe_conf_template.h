/**
 * @file ecat_foe_conf_template.h
 * @brief 全局工程配置模板 (依据 SAD-v0.1 第 4.1 节)
 *
 * 用户移植时复制为 ecat_foe_conf.h，置于包含路径，并对所有编译单元定义
 * ECAT_FOE_CONFIG_HEADER="ecat_foe_conf.h"。否则使用本模板。
 * 身份、目标及安全策略仍须在 ecat_foe_core_cfg_t 中配置。
 * 两个缓冲区尺寸须写为单个整数字面量（例如 148U、256U），所有编译单元
 * 必须使用相同的字面量；这些尺寸编码在配置 ABI 检查符号中。
 * 所有物理基地址与分区限额完全由本地硬编码管理，坚决不信任固件携带的任何物理地址。
 */

/* A direct template include must honor the same selection as core.h.  The
 * selection guard also permits a user config copied from this template. */
#if defined(ECAT_FOE_CONFIG_HEADER) && !defined(ECAT_FOE_CONFIG_SELECTING)
#define ECAT_FOE_CONFIG_SELECTING
#include ECAT_FOE_CONFIG_HEADER
#undef ECAT_FOE_CONFIG_SELECTING
#else
#ifndef ECAT_FOE_CONF_TEMPLATE_H
#define ECAT_FOE_CONF_TEMPLATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                               工程与硬件身份识别                           */
/* -------------------------------------------------------------------------- */
#define ECAT_FOE_PROJECT_ID                 0x10010001U
#define ECAT_FOE_PROJECT_NAME               "SERVO_DRV"
#define ECAT_FOE_HARDWARE_ID                0x00000001U
#define ECAT_FOE_LOCAL_HW_VER               0x0001U  /* uint16，与 efw 头的 hw_ver_min/max 一致 */

/* 最大受支持头为 v2 的 148 字节；必须 >= 148 且为 4 的倍数。 */
#define ECAT_FOE_HEADER_BUF_MAX             148U

/* -------------------------------------------------------------------------- */
/*                             目标升级使能开关                               */
/* -------------------------------------------------------------------------- */
#define ECAT_FOE_ENABLE_TARGET_APP          1  /* 目标 1: APP 固件 (字样: MCU) */
#define ECAT_FOE_ENABLE_TARGET_FPGA         0  /* 目标 2: FPGA 镜像 (字样: FPGA) */
#define ECAT_FOE_ENABLE_TARGET_SII_EEPROM   0  /* 目标 3: ESC SII EEPROM (字样: SII，高危默认严格关闭) */
#define ECAT_FOE_ENABLE_TARGET_USER_EEPROM  0  /* 目标 4: 外部用户数据 EEPROM (字样: EEPROM，默认关闭) */

/* 防版本回滚开关 (已拍板：默认关闭) */
#define ECAT_FOE_ANTI_ROLLBACK_ENABLE       0

/* Firmware signature verification is optional and fail-closed when enabled. */
#define ECAT_FOE_SIGNATURE_VERIFY_ENABLE    0

/* 容灾分区策略 (已拍板：默认单分区覆盖，A/B 双分区可选) */
#define ECAT_FOE_DUAL_BANK_ENABLE           0

/* -------------------------------------------------------------------------- */
/*                            写入物理对齐单位                                */
/* -------------------------------------------------------------------------- */
#define ECAT_FOE_APP_WRITE_UNIT             8U
#define ECAT_FOE_FPGA_WRITE_UNIT            256U
#define ECAT_FOE_SII_WRITE_UNIT             2U
#define ECAT_FOE_USER_EEPROM_WRITE_UNIT     4U

/* 最大写入对齐单位 (用于核心机残留缓冲区尺寸，取各启用目标 write_unit 的最大值) */
#define ECAT_FOE_MAX_WRITE_UNIT             256U

/* -------------------------------------------------------------------------- */
/*                             严格物理分区表                                 */
/* -------------------------------------------------------------------------- */
#define ECAT_FOE_APP_BASE_ADDR              0x08020000U
#define ECAT_FOE_APP_MAX_SIZE               (384U * 1024U)

#define ECAT_FOE_FPGA_BASE_ADDR             0x00100000U
#define ECAT_FOE_FPGA_MAX_SIZE              (1024U * 1024U)

#define ECAT_FOE_SII_EEPROM_MAX_SIZE        (4U * 1024U)

#define ECAT_FOE_USER_EEPROM_BASE_ADDR      0x00000000U
#define ECAT_FOE_USER_EEPROM_MAX_SIZE       (8U * 1024U)

/* 目标字样与 Target ID 映射项 */
typedef struct {
    const char *name_str;  /* 目标字样 (禁止含下划线) */
    uint16_t target_id;    /* 对应的 Target ID */
} ecat_foe_target_map_t;

/* 防回滚版本下限读取接口 (启用时由 Platform/Port 实现)。 */
int port_get_version_floor(uint16_t target_id, uint32_t *floor_ver);
/* 独立下限写入接口仅供平台管理使用；核心提交改由目标驱动联合事务完成。 */
int port_set_version_floor(uint16_t target_id, uint32_t new_floor_ver);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_FOE_CONF_TEMPLATE_H */
#endif /* configuration selection */
