/**
 * @file bootloader_config.h
 * @brief 最小 Bootloader 板级静态配置头文件
 *
 * 本头文件定义 MCU Bootloader 工程的板级静态硬件参数、工程识别符与 Flash 分区边界。
 *
 * 【核心设计契约与安全原则】：
 * 1. 物理写入基地址只由本地配置独家裁决：
 *    Flash 目标存储驱动在执行擦除与写入时，物理地址严格由本地宏 APP_FLASH_BASE_ADDR
 *    与本地偏移计算得出。严禁信任或直接使用远程 FoE WRQ 报文中的 dest_start_address、
 *    文件名或 .efw 固件容器内附带的任何外部地址，从物理机制上杜绝恶意或损坏固件越界
 *    踩踏 Bootloader 自身分区 (0x08000000 ~ 0x0801FFFF)。
 * 2. 静态内存与零动态分配：
 *    所有配置均为编译期宏与静态结构体，不依赖 malloc/free。
 * 3. 动态文件名协议保持：
 *    支持标准动态文件名格式：<PROJECT_NAME>_<TARGET>_V<M>.<m>.<p>.<b|r>.efw。
 */

#ifndef BOOTLOADER_CONFIG_H
#define BOOTLOADER_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                           板级工程与硬件识别符                             */
/* -------------------------------------------------------------------------- */

/** 本地工程唯一标识 (32-bit ID) */
#define BOOTLOADER_PROJECT_ID          0x10010001U

/** 本地工程名称字符串 (与 .efw 容器及文件名动态切分匹配) */
#define BOOTLOADER_PROJECT_NAME        "SERVO_DRV"

/** 本地硬件板卡版本唯一标识 */
#define BOOTLOADER_HARDWARE_ID         0x00000001U

/** 允许运行的硬件版本范围 [HW_VER_MIN, HW_VER_MAX] */
#define BOOTLOADER_HW_VER_MIN          1U
#define BOOTLOADER_HW_VER_MAX          1U

/** 本机当前物理硬件版本号 */
#define BOOTLOADER_LOCAL_HW_VER        1U

/* -------------------------------------------------------------------------- */
/*                                目标存储配置                                */
/* -------------------------------------------------------------------------- */

/** 目标 1: 主 MCU 片内应用固件 */
#define BOOTLOADER_TARGET_MCU_ID       1U
#define BOOTLOADER_TARGET_MCU_NAME     "MCU"
#define BOOTLOADER_TARGET_MCU_FILENAME "SERVO_DRV_MCU.efw"

/* -------------------------------------------------------------------------- */
/*                 Flash 物理存储分区划分 (严格由本地配置裁决)                */
/* -------------------------------------------------------------------------- */

/** Bootloader 自身物理 Flash 起始基地址与保留尺寸 (只读受保护区) */
#define BOOTLOADER_FLASH_BASE          0x08000000U
#define BOOTLOADER_FLASH_SIZE          (128U * 1024U)  /* 128 KB */

/** 应用固件 (App Partition) 物理 Flash 起始基地址 (写入基准) */
#define APP_FLASH_BASE_ADDR            0x08020000U

/** 应用固件分区最大允许容量 (配额上限，防止越界) */
#define APP_FLASH_MAX_SIZE             (256U * 1024U)  /* 256 KB */

/** Flash 物理扇区最小擦除粒度 (字节) */
#define APP_FLASH_SECTOR_SIZE          4096U           /* 4 KB 扇区 */

/** Flash 物理最小写入对齐单位 (字节) */
#define APP_FLASH_WRITE_UNIT           8U              /* 64-bit 对齐写 */

/** 固件有效标志魔数 (写入分区末尾或头部元数据) */
#define APP_FLASH_VALID_MAGIC          0x5A5AA5A5U
#define APP_FLASH_INVALID_MAGIC        0x00000000U

#ifdef __cplusplus
}
#endif

#endif /* BOOTLOADER_CONFIG_H */
