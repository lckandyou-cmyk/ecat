/**
 * @file ecat_transfer.h
 * @brief 嵌入式多通道双向传输管理接口 (Bidirectional Transfer Manager)
 *
 * 特性:
 * 1. 双向能力 (Bidirectional):
 *    - 导入/写入 (IMPORT): 支持固件安全升级与普通数据流写入
 *    - 导出/回读 (EXPORT): 支持日志导出、参数回读与固件备份
 * 2. 目标解耦: 基于 ecat_target_registry_t，不绑定任何硬编码目标
 * 3. 权限与通道隔离: 结合 ACL 验证 (transport_id, target_id, direction)
 * 4. 零动态分配 (Zero malloc): 纯静态控制块与单租约互斥令牌机制
 */

#ifndef ECAT_TRANSFER_H
#define ECAT_TRANSFER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ecat_upgrade_session.h"
#include "ecat_target_registry.h"
#include "ecat_foe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 传输结果码 */
typedef enum {
    ECAT_TRANSFER_COMPLETE = 0,     /* 操作完成 (已达到文件尾或单步全部完成) */
    ECAT_TRANSFER_CONTINUE,         /* 成功且后续还有数据 (针对分块读取) */
    ECAT_TRANSFER_PENDING,          /* 异步执行中 (需轮询) */
    ECAT_TRANSFER_FAILED,           /* 内部或驱动错误 */
    ECAT_TRANSFER_BUSY,             /* 传输引擎已被其他通道占用 */
    ECAT_TRANSFER_INVALID,          /* 参数非法 */
    ECAT_TRANSFER_STALE,            /* 令牌失效或所有者不匹配 */
    ECAT_TRANSFER_DENIED            /* 权限不足或未授权操作 (ACL 拦截) */
} ecat_transfer_result_t;

/* 传输方向 */
typedef enum {
    ECAT_TRANSFER_DIR_IMPORT = 1,   /* 外部向设备写入 / 固件升级 / 配置导入 */
    ECAT_TRANSFER_DIR_EXPORT = 2    /* 设备向外部发送 / 日志回读 / 数据导出 */
} ecat_transfer_dir_t;

/* 错误详细码 */
typedef enum {
    ECAT_TRANSFER_ERR_NONE = 0,
    ECAT_TRANSFER_ERR_INVALID_ARG,
    ECAT_TRANSFER_ERR_TARGET_NOT_FOUND,
    ECAT_TRANSFER_ERR_PERMISSION_DENIED,
    ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED,
    ECAT_TRANSFER_ERR_DRIVER_INIT,
    ECAT_TRANSFER_ERR_DRIVER_INVALIDATE,
    ECAT_TRANSFER_ERR_DRIVER_ERASE,
    ECAT_TRANSFER_ERR_DRIVER_WRITE,
    ECAT_TRANSFER_ERR_DRIVER_READ,
    ECAT_TRANSFER_ERR_DRIVER_FINALIZE,
    ECAT_TRANSFER_ERR_CORE_UPGRADE
} ecat_transfer_err_t;

/* 传输请求结构体 */
typedef struct {
    uint16_t transport_id;          /* 发起请求的传输通道 ID */
    uint16_t target_id;             /* 目标区块 ID */
    ecat_transfer_dir_t direction;  /* 传输方向 (IMPORT / EXPORT) */
    uint32_t expected_size;         /* 预期传输字节数 (0 表示按驱动/文件尾结束) */
    uint32_t fw_version;            /* 固件版本号 (仅固件升级模式生效) */
} ecat_transfer_request_t;

/* 双向传输管理器上下文 */
typedef struct {
    ecat_target_registry_t *registry;   /* 目标注册表指针 */
    ecat_foe_core_t *core;              /* 可选: 底层 FoE 固件升级机 (若不升级固件可为 NULL) */
    ecat_upgrade_session_t session;     /* 互斥会话与租约令牌 */
    ecat_upgrade_session_t *session_ptr; /* 共享仲裁器会话指针 */

    /* 当前激活传输状态 */
    const ecat_target_desc_t *target;   /* 当前目标描述符 */
    uint16_t current_transport_id;      /* 当前持有者通道 ID */
    ecat_transfer_dir_t direction;      /* 当前传输方向 */
    uint32_t current_offset;            /* 当前已处理偏移量 */
    uint32_t total_size;                /* 总大小 (导出时为数据量，导入时为限额) */
    uint32_t expected_size;             /* 声明的预期总大小 (0 表示未知/流式传输) */
    uint32_t erased_watermark;          /* 已擦除高水位线 */
    uint32_t running_crc32;             /* 传输流实时 CRC32 校验码 */
    bool active;                        /* 会话是否处于激活状态 */
    bool driver_initialized;            /* 驱动是否成功初始化 (用于保证 abort 契约) */
    bool core_started;                  /* 是否已向底层 core 发起请求 */
    uint32_t saved_core_max_payload;    /* 会话期间暂存的 core 原始容量，防止永久污染配置 */
    uint32_t saved_core_target_idx;     /* 暂存容量对应的目标索引 */
    bool core_cap_overridden;           /* 是否暂存并临时重写了 core 容量限额 */

    ecat_transfer_err_t last_error;     /* 最近一次错误码 */
} ecat_transfer_t;

/* -------------------------------------------------------------------------- */
/*                                API 接口声明                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化双向传输管理器
 * @param t 管理器上下文指针
 * @param reg 目标注册表指针 (必须非空)
 * @param core 可选固件升级核心状态机 (若不执行固件安全升级可传入 NULL)
 * @return 0 成功，-1 参数非法
 */
int ecat_transfer_init(ecat_transfer_t *t, ecat_target_registry_t *reg, ecat_foe_core_t *core);

/**
 * @brief 发起双向传输会话 (导入或导出)
 * @param t 管理器上下文指针
 * @param owner 调用方唯一标识指针 (通道实例地址)
 * @param request 传输请求参数
 * @param token 传出获得的独占会话令牌
 * @return 传输结果码
 */
ecat_transfer_result_t ecat_transfer_begin(
    ecat_transfer_t *t,
    const void *owner,
    const ecat_transfer_request_t *request,
    uint64_t *token);

/**
 * @brief 数据导出分块读取 (EXPORT 模式)
 * @param t 管理器上下文指针
 * @param owner 调用方唯一标识
 * @param token 会话令牌
 * @param out_buf 接收缓冲区
 * @param buf_capacity 缓冲区容量
 * @param actual_read 传出实际读取到的字节数
 * @return ECAT_TRANSFER_COMPLETE: 数据已全部读完;
 *         ECAT_TRANSFER_CONTINUE: 成功读取一块，后续仍有数据;
 *         其他表示错误
 */
ecat_transfer_result_t ecat_transfer_read(
    ecat_transfer_t *t,
    const void *owner,
    uint64_t token,
    uint8_t *out_buf,
    size_t buf_capacity,
    size_t *actual_read);

/**
 * @brief 数据导入分块写入 (IMPORT 模式)
 * @param t 管理器上下文指针
 * @param owner 调用方唯一标识
 * @param token 会话令牌
 * @param data 待写入数据
 * @param length 数据长度
 * @return ECAT_TRANSFER_COMPLETE / 错误码
 */
ecat_transfer_result_t ecat_transfer_write(
    ecat_transfer_t *t,
    const void *owner,
    uint64_t token,
    const uint8_t *data,
    size_t length);

/**
 * @brief 正常完成传输会话并释放锁
 * @param t 管理器上下文指针
 * @param owner 调用方唯一标识
 * @param token 会话令牌
 * @return 传输结果码
 */
ecat_transfer_result_t ecat_transfer_finish(
    ecat_transfer_t *t,
    const void *owner,
    uint64_t token);

/**
 * @brief 取消/异常中止传输会话
 * @param t 管理器上下文指针
 * @param owner 调用方唯一标识
 * @param token 会话令牌
 * @return 传输结果码
 */
ecat_transfer_result_t ecat_transfer_cancel(
    ecat_transfer_t *t,
    const void *owner,
    uint64_t token);

/**
 * @brief 获取当前会话已计算的 IEEE 802.3 CRC32
 */
uint32_t ecat_transfer_get_crc32(const ecat_transfer_t *t);

/**
 * @brief 获取当前会话已传输的字节总量
 */
uint32_t ecat_transfer_get_bytes_transferred(const ecat_transfer_t *t);

/**
 * @brief 获取可导出的总字节数 (仅 EXPORT 模式有效)
 */
uint32_t ecat_transfer_get_total_export_size(const ecat_transfer_t *t);

/**
 * @brief 获取最近一次错误代码
 */
ecat_transfer_err_t ecat_transfer_get_last_error(const ecat_transfer_t *t);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_TRANSFER_H */
