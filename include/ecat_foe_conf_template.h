/**
 * @file ecat_foe_conf_template.h
 * @brief EtherCAT FoE 固件升级库全局工程配置模板
 *
 * 用户移植时，复制此文件为 ecat_foe_conf.h 并置于工程包含路径中。
 * 项目 ID、硬件 ID、目标使能开关、各目标基地址与分区大小皆集中于本文件管理。
 */

#ifndef ECAT_FOE_CONF_H
#define ECAT_FOE_CONF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                               工程与硬件识别                               */
/* -------------------------------------------------------------------------- */
/** @brief 工程唯一识别数字 ID (必须与 .efw 文件头匹配) */
#define ECAT_FOE_CONF_PROJECT_ID          0x10010001U

/** @brief 工程名称字符串 (必须与 .efw 文件名及文件头严格对应) */
#define ECAT_FOE_CONF_PROJECT_NAME        "SERVO_DRV"

/** @brief 硬件平台识别 ID (防止不同板卡误刷相同工程代码) */
#define ECAT_FOE_CONF_HARDWARE_ID         0x00000001U

/* -------------------------------------------------------------------------- */
/*                               升级目标使能开关                             */
/* -------------------------------------------------------------------------- */
/** @brief 是否允许升级 MCU 应用程序 (APP) */
#define ECAT_FOE_ENABLE_TARGET_APP        1

/** @brief 是否允许升级 FPGA/CPLD 逻辑固件 */
#define ECAT_FOE_ENABLE_TARGET_FPGA       0

/** @brief 是否允许升级 ESC SII EEPROM 固件 */
#define ECAT_FOE_ENABLE_TARGET_EEPROM     0

/* -------------------------------------------------------------------------- */
/*                       分区配置 (Bootloader 绝对物理分区)                    */
/* 核心库只认此处硬编码的基地址与限额，坚决忽略固件文件中携带的任何地址信息 */
/* -------------------------------------------------------------------------- */
/** @brief APP 固件烧录物理基地址 */
#define ECAT_FOE_APP_BASE_ADDR            0x08020000U
/** @brief APP 固件最大允许大小 (例如 384KB) */
#define ECAT_FOE_APP_MAX_SIZE             (384U * 1024U)

/** @brief FPGA 固件物理基地址 (例如 SPI Flash 扇区偏移) */
#define ECAT_FOE_FPGA_BASE_ADDR           0x00100000U
/** @brief FPGA 固件最大允许大小 (例如 1MB) */
#define ECAT_FOE_FPGA_MAX_SIZE            (1024U * 1024U)

/** @brief EEPROM 固件物理基地址 */
#define ECAT_FOE_EEPROM_BASE_ADDR         0x00000000U
/** @brief EEPROM 固件最大允许大小 (例如 4KB) */
#define ECAT_FOE_EEPROM_MAX_SIZE          (4U * 1024U)

/* -------------------------------------------------------------------------- */
/*                               高级与超时参数                               */
/* -------------------------------------------------------------------------- */
/** @brief 单个 FoE 数据包最大期待尺寸 (通常等于 Bootstrap Mailbox 尺寸，如 256/512/1024) */
#define ECAT_FOE_MAX_PACKET_SIZE          1024U

/** @brief Flash 耗时擦除时的 Busy 重试轮询上限 (防止主站过早判定超时) */
#define ECAT_FOE_BUSY_MAX_RETRIES         200U

#ifdef __cplusplus
}
#endif

#endif /* ECAT_FOE_CONF_H */
