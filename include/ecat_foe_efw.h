/**
 * @file ecat_foe_efw.h
 * @brief EtherCAT .efw 固件容器格式解析与核验接口定义 (v0.1)
 *
 * 严格遵循 docs/spec/efw-format-v0.1.md 规范：
 * - 纯 C99 实现，零动态内存分配 (无 malloc)
 * - 小端序逐字节安全反序列化，不依赖结构体物理对齐与主机字节序
 * - 三阶段解析顺序：阶段 1 探测、阶段 2 头部装配与 CRC32、阶段 3 全量元数据安全核验
 * - 独立增量 IEEE 802.3 CRC32 计算接口
 */

#ifndef ECAT_FOE_EFW_H
#define ECAT_FOE_EFW_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                               常量与宏定义                                 */
/* -------------------------------------------------------------------------- */

/** @brief 容器固定魔数: "ECFW" 小端序表示 (0x57464345) */
#define ECAT_FOE_EFW_MAGIC                  0x57464345U

/** @brief 当前规范版本号 */
#define ECAT_FOE_EFW_FORMAT_VERSION         1U

/** @brief 头部最小长度 (字节) */
#define ECAT_FOE_EFW_HEADER_MIN_LEN         64U

/** @brief 扩展保留区长度 (字节，偏移 0x28 - 0x3B) */
#define ECAT_FOE_EFW_RESERVED_LEN           20U

/** @brief 阶段 1 探测所需最小字节数 */
#define ECAT_FOE_EFW_STAGE1_PROBE_LEN       8U

/* 升级目标定义 (efw-format v0.1 第 5 节) */
#define ECAT_FOE_TARGET_MCU                 0x0001U  /**< MCU 固件 (字样: MCU) */
#define ECAT_FOE_TARGET_FPGA                0x0002U  /**< FPGA 镜像 (字样: FPGA) */
#define ECAT_FOE_TARGET_SII                 0x0003U  /**< ESC SII EEPROM (字样: SII) */
#define ECAT_FOE_TARGET_EEPROM              0x0004U  /**< 外部用户数据 EEPROM (字样: EEPROM) */
#define ECAT_FOE_TARGET_COUNT               4U

/* -------------------------------------------------------------------------- */
/*                               错误码枚举                                   */
/* -------------------------------------------------------------------------- */

typedef enum {
    ECAT_FOE_EFW_OK = 0,

    /* 通用调用错误 */
    ECAT_FOE_EFW_ERR_INVALID_ARG,             /**< 空指针或非法参数 */
    ECAT_FOE_EFW_ERR_BUFFER_TOO_SMALL,        /**< 输入缓冲区长度不足 */

    /* 阶段 1 探测错误 (前 8 字节) */
    ECAT_FOE_EFW_ERR_MAGIC,                   /**< 魔数错误 (magic != 0x57464345) */
    ECAT_FOE_EFW_ERR_FORMAT_VERSION,          /**< 规范版本号不支持 (format_version != 1) */
    ECAT_FOE_EFW_ERR_HEADER_LEN_MIN,          /**< 头部长度小于 64 字节 */
    ECAT_FOE_EFW_ERR_HEADER_LEN_MAX,          /**< 头部长度超过本地允许上限 */
    ECAT_FOE_EFW_ERR_HEADER_LEN_ALIGN,        /**< 头部长度不是 4 的倍数 */

    /* 阶段 2 校验错误 */
    ECAT_FOE_EFW_ERR_HEADER_CRC,              /**< Header CRC32 计算比对不匹配 */

    /* 阶段 3 "擦除前必须全过"元数据核验错误 (efw-format 第 4.2 节) */
    ECAT_FOE_EFW_ERR_FLAGS_NONZERO,           /**< flags 包含未定义的非零标志位 */
    ECAT_FOE_EFW_ERR_RESERVED_NONZERO,        /**< reserved 扩展保留区包含非零字节 */
    ECAT_FOE_EFW_ERR_PROJECT_ID_MISMATCH,     /**< project_id 与从站本地配置不符 */
    ECAT_FOE_EFW_ERR_HARDWARE_ID_MISMATCH,    /**< hardware_id 与从站本地配置不符 */
    ECAT_FOE_EFW_ERR_HW_VER_RANGE_INVALID,    /**< 硬件版本区间倒置 (hw_ver_min > hw_ver_max) */
    ECAT_FOE_EFW_ERR_HW_VER_NOT_SUPPORTED,    /**< 当前从站本地硬件版本不在支持区间内 */
    ECAT_FOE_EFW_ERR_TARGET_UNKNOWN,          /**< target_id 为未知目标 */
    ECAT_FOE_EFW_ERR_TARGET_NOT_ENABLED,      /**< target_id 在从站本地未开启使能 */
    ECAT_FOE_EFW_ERR_PAYLOAD_TOO_LARGE,       /**< payload_length 超过本地分区容量上限 */
    ECAT_FOE_EFW_ERR_MIN_ALLOW_VER_INVALID,   /**< 允许运行下限大于固件版本 (min_allow_version > fw_version) */
    ECAT_FOE_EFW_ERR_ANTI_ROLLBACK            /**< 防回滚拦截: 固件版本低于本地持久化下限 */
} ecat_foe_efw_err_t;

/* -------------------------------------------------------------------------- */
/*                               数据结构定义                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief 解析后的 .efw 头部元数据结构体
 * 纯数据载体，字段取值在反序列化时完成小端转主机序转换。
 */
typedef struct {
    uint32_t magic;                              /**< 0x00: 魔数 (0x57464345) */
    uint16_t format_version;                     /**< 0x04: 规范格式版本 (1) */
    uint16_t header_length;                      /**< 0x06: 头部总字节数 (>= 64, 4 的倍数) */
    uint32_t project_id;                         /**< 0x08: 目标工程唯一标识 ID */
    uint32_t hardware_id;                        /**< 0x0C: 目标硬件板型标识 ID */
    uint16_t hw_ver_min;                         /**< 0x10: 支持的最低硬件版本号 */
    uint16_t hw_ver_max;                         /**< 0x12: 支持的最高硬件版本号 */
    uint16_t target_id;                          /**< 0x14: 升级目标标识 ID (1..4) */
    uint16_t flags;                              /**< 0x16: 升级标志位 (必须为 0) */
    uint32_t fw_version;                         /**< 0x18: 固件版本号数值 (Major<<24|Minor<<16|Patch<<8|Build) */
    uint32_t min_allow_version;                  /**< 0x1C: 允许运行的最低版本号 */
    uint32_t payload_length;                     /**< 0x20: 有效负载长度 (字节数) */
    uint32_t payload_crc32;                      /**< 0x24: 有效负载 IEEE 802.3 CRC32 校验码 */
    uint8_t  reserved[ECAT_FOE_EFW_RESERVED_LEN];/**< 0x28 - 0x3B: 扩展保留区 (必须全为 0x00) */
    uint32_t header_crc32;                       /**< header_length - 4: 头部 CRC32 校验码 */
} ecat_foe_efw_header_t;

/**
 * @brief 单目标本地使能与限额配置
 */
typedef struct {
    bool     enabled;                            /**< 是否使能该目标 */
    uint32_t max_payload_size;                   /**< 分区最大允许容量 (字节)，0 表示未配置上限 */
    uint32_t version_floor;                      /**< 防回滚当前版本下限 */
} ecat_foe_efw_target_cfg_t;

/**
 * @brief 从站本地静态配置结构体 (阶段 3 校验入参)
 */
typedef struct {
    uint32_t project_id;                         /**< 本地工程 ID */
    uint32_t hardware_id;                        /**< 本地硬件 ID */
    uint16_t local_hw_ver;                       /**< 本地硬件版本号 (uint16) */
    bool     anti_rollback_enable;               /**< 是否启用防版本回滚校验 */
    ecat_foe_efw_target_cfg_t targets[ECAT_FOE_TARGET_COUNT]; /**< 目标 1..4 配置 (索引 target_id - 1) */
} ecat_foe_efw_config_t;

/* -------------------------------------------------------------------------- */
/*                               CRC32 增量接口                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化 CRC32 上下文
 * 按照 IEEE 802.3 规范，初始值置为 0xFFFFFFFFU。
 */
void ecat_foe_crc32_init(uint32_t *crc);

/**
 * @brief 增量追加数据计算 CRC32
 * @param crc  CRC 上下文指针
 * @param data 输入数据字节流
 * @param len  数据字节数
 */
void ecat_foe_crc32_update(uint32_t *crc, const uint8_t *data, size_t len);

/**
 * @brief 完成 CRC32 计算并输出最终校验码
 * 输出前执行异或 0xFFFFFFFFU。
 * @param crc CRC 上下文指针 (内部值保持更新)
 * @return 最终 32 位校验码
 */
uint32_t ecat_foe_crc32_final(const uint32_t *crc);

/**
 * @brief 单步计算完整数据块的 CRC32 校验码
 */
uint32_t ecat_foe_crc32_calc(const uint8_t *data, size_t len);

/* -------------------------------------------------------------------------- */
/*                             三阶段解析核验接口                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief 阶段 1：前 8 字节轻量探测
 *
 * 检查项：
 * - magic == 0x57464345
 * - format_version == 1
 * - header_length >= 64
 * - header_length <= max_header_len
 * - header_length % 4 == 0
 *
 * @param buf            数据流起始缓冲指针 (至少 8 字节)
 * @param len            输入数据长度
 * @param max_header_len 本地允许的最大头部装配长度 (例如 128)
 * @param out_header_len 输出解析得到的 header_length (不可为 NULL)
 * @return ECAT_FOE_EFW_OK 探测成功，否则返回对应细分错误码
 */
ecat_foe_efw_err_t ecat_foe_efw_parse_stage1(
    const uint8_t *buf,
    size_t len,
    uint16_t max_header_len,
    uint16_t *out_header_len
);

/**
 * @brief 阶段 2：头部整体装配与 CRC 校验
 *
 * 在流式缓冲攒齐完整的 header_length 字节后调用：
 * - 从 header_length - 4 处读取 Header CRC32；
 * - 对前 header_length - 4 字节全部数据计算并比对 CRC32；
 * - 逐字节小端序反序列化基础字段至 out_hdr。
 *
 * @param buf     包含完整 header_length 字节的头部数据流
 * @param len     数据长度 (必须 >= header_length)
 * @param out_hdr 输出反序列化后的头部结构体 (不可为 NULL)
 * @return ECAT_FOE_EFW_OK 校验装配成功，否则返回对应细分错误码
 */
ecat_foe_efw_err_t ecat_foe_efw_parse_stage2(
    const uint8_t *buf,
    size_t len,
    ecat_foe_efw_header_t *out_hdr
);

/**
 * @brief 阶段 3：全量元数据安全核验 ("擦除前必须全过"清单)
 *
 * 严格按照 efw-format-v0.1.md 第 4.2 节清单执行核验：
 * 1. magic 严格等于 0x57464345
 * 2. format_version 严格等于 1
 * 3. header_length >= 64 且为 4 的倍数
 * 4. flags 严格等于 0
 * 5. reserved 区域 (20 字节) 全为 0x00
 * 6. project_id 与本地配置一致
 * 7. hardware_id 与本地配置一致
 * 8. hw_ver_min <= hw_ver_max 且 local_hw_ver 落在 [hw_ver_min, hw_ver_max] 内
 * 9. target_id 为已知目标 (1..4) 且在本地配置中已开启使能
 * 10. payload_length 未超过本地分配的分区容量上限
 * 11. min_allow_version <= fw_version
 * 12. 若开启防回滚，fw_version >= version_floor
 *
 * @param hdr 阶段 2 装配成功的头部结构体指针
 * @param cfg 本地配置参数指针
 * @return ECAT_FOE_EFW_OK 全部通过，允许进入擦除与写入；否则返回对应细分错误码
 */
ecat_foe_efw_err_t ecat_foe_efw_parse_stage3(
    const ecat_foe_efw_header_t *hdr,
    const ecat_foe_efw_config_t *cfg
);

/**
 * @brief 错误码转换为人类可读描述字符串
 */
const char *ecat_foe_efw_err_to_str(ecat_foe_efw_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_FOE_EFW_H */
