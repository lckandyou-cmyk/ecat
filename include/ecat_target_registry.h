/**
 * @file ecat_target_registry.h
 * @brief 嵌入式通用目标区块注册表与权限控制矩阵 (Target Registry & ACL)
 *
 * 特性:
 * 1. 零动态分配 (Zero malloc): 纯静态内存，编译期或初始化期定义上限。
 * 2. 目标解耦: 支持任意数量区块（固件区、参数区、日志区、私有配置区等），
 *    突破硬编码 4 目标限制。
 * 3. 区分区块类型: 固件区 (EFW/签名/防回滚) vs 数据区 (原始数据流/参数/日志)。
 * 4. 细粒度双向权限: 每个区块独立配置 READ(导出) / WRITE(导入) 权限。
 * 5. 通道访问控制矩阵 (ACL): 可按 (transport_id, target_id) 精确授权。
 */

#ifndef ECAT_TARGET_REGISTRY_H
#define ECAT_TARGET_REGISTRY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ecat_foe_target.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ECAT_TARGET_REGISTRY_MAX_BLOCKS
#define ECAT_TARGET_REGISTRY_MAX_BLOCKS 16U
#endif

#ifndef ECAT_TARGET_REGISTRY_MAX_ACL_RULES
#define ECAT_TARGET_REGISTRY_MAX_ACL_RULES 32U
#endif

#include "ecat_target_registry_abi.h"

void ECAT_TARGET_REGISTRY_ABI_SYMBOL(void);
#if defined(__has_attribute)
#if __has_attribute(used) && __has_attribute(retain)
__attribute__((used, retain)) static void (*const ecat_target_registry_abi_reference)(void) = ECAT_TARGET_REGISTRY_ABI_SYMBOL;
#else
#error "Target registry ABI checking requires compiler support for used and retain attributes"
#endif
#else
#error "Target registry ABI checking requires compiler support for used and retain attributes"
#endif

#define ECAT_TARGET_NAME_MAX_LEN 16U

/* 权限标志位 */
#define ECAT_PERM_READ   (1U << 0)  /* 允许导出 / 回读 (Export) */
#define ECAT_PERM_WRITE  (1U << 1)  /* 允许导入 / 升级写入 (Import / Upgrade) */
#define ECAT_PERM_RW     (ECAT_PERM_READ | ECAT_PERM_WRITE)

/* 区块类型 */
typedef enum {
    ECAT_TARGET_TYPE_FIRMWARE = 0,  /* 固件镜像区块: 必须通过 EFW 容器头部/签名/版本防回滚核验 */
    ECAT_TARGET_TYPE_DATA           /* 普通数据区块: 参数、日志、配置、字库等纯数据流 */
} ecat_target_type_t;

/* 真正非阻塞异步数据操作结果码 */
typedef enum {
    ECAT_ASYNC_OP_FAILED = -1,
    ECAT_ASYNC_OP_COMPLETE = 0,
    ECAT_ASYNC_OP_PENDING = 1
} ecat_async_op_result_t;

/* 真正非阻塞异步数据操作类型 */
typedef enum {
    ECAT_ASYNC_DATA_OP_INIT = 0,
    ECAT_ASYNC_DATA_OP_INVALIDATE,
    ECAT_ASYNC_DATA_OP_ERASE_TO,
    ECAT_ASYNC_DATA_OP_WRITE,
    ECAT_ASYNC_DATA_OP_READ,
    ECAT_ASYNC_DATA_OP_FINALIZE,
    ECAT_ASYNC_DATA_OP_READ_OPEN,
    ECAT_ASYNC_DATA_OP_READ_CLOSE
} ecat_async_data_op_kind_t;

/* 真正非阻塞异步数据操作描述符 (生命周期保持稳定直至 COMPLETE 或 FAILED) */
typedef struct {
    ecat_async_data_op_kind_t kind; /* 操作种类 */
    uint32_t offset;                /* 操作起始偏移量 (WRITE/READ) */
    uint32_t need_end;              /* 需要擦除到的结束边界 (ERASE_TO) */
    uint32_t erased_end;            /* 驱动反馈的实际已擦除边界 (ERASE_TO) */
    uint32_t length;                /* 缓冲区数据长度 */
    uint8_t *buffer;                /* 数据暂存区指针 (WRITE 输入暂存或 READ 输出暂存) */
    size_t actual_length;           /* 驱动反馈的实际处理字节数 (READ/WRITE) */
} ecat_async_data_op_t;

/* 真正非阻塞异步数据驱动操作接口 */
typedef struct {
    /**
     * @brief 启动异步操作
     * @param user_ctx 用户自定义上下文
     * @param op 操作描述符
     * @return COMPLETE(0) 立即完成, PENDING(1) 后台执行中需轮询, FAILED(-1) 失败
     */
    ecat_async_op_result_t (*start)(void *user_ctx, ecat_async_data_op_t *op);

    /**
     * @brief 轮询异步操作进度
     * @param user_ctx 用户自定义上下文
     * @param op 操作描述符
     * @return COMPLETE(0) 完成, PENDING(1) 仍需继续轮询, FAILED(-1) 失败
     */
    ecat_async_op_result_t (*poll)(void *user_ctx, ecat_async_data_op_t *op);

    /**
     * @brief 取消/中止后台异步操作并确保硬件释放暂存缓冲访问
     * @param user_ctx 用户自定义上下文
     */
    void (*abort)(void *user_ctx);
} ecat_async_data_ops_t;

/* 扩展目标驱动定义: 包含标准写入回调、可选导出尺寸获取、只读生命周期回调以及真正非阻塞异步驱动 */
typedef struct {
    ecat_foe_target_driver_t io;
    /**
     * @brief 获取可导出的有效数据大小 (字节)
     * @param user_ctx 用户上下文
     * @return 返回实际有效数据量；若为 NULL 则默认使用区块配置的 max_size
     */
    uint32_t (*get_data_size)(void *user_ctx);

    /**
     * @brief 可选只读打开回调 (EXPORT 开始时调用)
     * @note 仅用于获取只读资源与互斥锁。read_open 与 read_close 必须成对提供；
     *       若两者皆为 NULL 且 io.init == NULL，则作为无生命周期纯 read 驱动放行；
     *       若驱动包含 io.init 但缺少 read_close，为防止资源泄漏或误调 abort 损坏介质镜像，
     *       导出请求将在 begin 阶段被直接拒绝 (返回 ECAT_TRANSFER_ERR_DRIVER_INIT)，
     *       绝不回退调用写生命周期的 io.init。
     * @param user_ctx 用户自定义上下文指针
     * @return 0 成功，非 0 失败
     */
    int (*read_open)(void *user_ctx);

    /**
     * @brief 可选只读关闭回调 (EXPORT 结束或取消时调用)
     * @note 必须与 read_open 成对提供。仅释放只读会话资源/锁，绝不触碰镜像有效性标记或触发写入失效。
     * @param user_ctx 用户自定义上下文指针
     */
    void (*read_close)(void *user_ctx);

    /**
     * @brief 可选真正非阻塞异步数据驱动接口 (针对 DATA 类型的异步操作)
     */
    const ecat_async_data_ops_t *async_ops;
} ecat_target_driver_t;

/* 单个目标区块描述符 */
typedef struct {
    uint16_t target_id;                             /* 目标唯一标识符 (非 0) */
    char name[ECAT_TARGET_NAME_MAX_LEN];            /* 字符标识 (如 "mcu", "fpga", "param", "log") */
    ecat_target_type_t type;                        /* 区块类型 (固件 / 数据) */
    uint32_t max_size;                              /* 本地物理容量上限 (字节，硬编码边界) */
    uint8_t permissions;                            /* 目标自身允许的权限位 (ECAT_PERM_READ/WRITE) */
    bool enabled;                                   /* 是否启用 */
    bool asynchronous;                              /* 是否为异步处理目标 */
    ecat_target_driver_t driver;                    /* 驱动函数指针集合 */
    void *user_ctx;                                 /* 驱动用户上下文指针 */
} ecat_target_desc_t;

/* 访问控制规则 (ACL) */
typedef struct {
    uint16_t transport_id;  /* 传输通道 ID (0 表示通配所有通道) */
    uint16_t target_id;     /* 目标区块 ID (0 表示通配所有区块) */
    uint8_t allowed_perms;  /* 该规则授予的权限掩码 */
} ecat_target_acl_rule_t;

/* 目标注册表上下文 */
typedef struct {
    ecat_target_desc_t targets[ECAT_TARGET_REGISTRY_MAX_BLOCKS];
    size_t target_count;

    ecat_target_acl_rule_t acl_rules[ECAT_TARGET_REGISTRY_MAX_ACL_RULES];
    size_t acl_rule_count;
    bool enforce_acl;       /* 是否强制 ACL 白名单过滤；若为 false，则直接使用 target 自身的 permissions */
} ecat_target_registry_t;

/* -------------------------------------------------------------------------- */
/*                                API 接口声明                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化目标注册表
 * @param reg 注册表指针
 * @return 0 成功，-1 参数为空
 */
int ecat_target_registry_init(ecat_target_registry_t *reg);

/**
 * @brief 注册一个存储目标区块
 * @param reg 注册表指针
 * @param desc 区块描述符指针 (内部会浅拷贝内容)
 * @return 0 成功，负数表示失败 (如 ID 重复、参数非法或容量已满)
 */
int ecat_target_registry_add(ecat_target_registry_t *reg, const ecat_target_desc_t *desc);

/**
 * @brief 按 target_id 查找目标描述符
 * @param reg 注册表指针
 * @param target_id 目标 ID
 * @return 指向已注册描述符的只读指针，未找到返回 NULL
 */
const ecat_target_desc_t *ecat_target_registry_find_by_id(
    const ecat_target_registry_t *reg, uint16_t target_id);

/**
 * @brief 按 name 查找目标描述符
 * @param reg 注册表指针
 * @param name 目标名称 (以 \0 结尾)
 * @return 指向已注册描述符的只读指针，未找到返回 NULL
 */
const ecat_target_desc_t *ecat_target_registry_find_by_name(
    const ecat_target_registry_t *reg, const char *name);

/**
 * @brief 添加一条访问控制规则 (ACL)
 * @param reg 注册表指针
 * @param transport_id 传输通道 ID (0 表示通配所有传输通道)
 * @param target_id 目标区块 ID (0 表示通配所有目标区块)
 * @param allowed_perms 允许的操作位 (ECAT_PERM_READ, ECAT_PERM_WRITE, ECAT_PERM_RW)
 * @return 0 成功，负数表示失败
 */
int ecat_target_registry_add_acl(ecat_target_registry_t *reg,
    uint16_t transport_id, uint16_t target_id, uint8_t allowed_perms);

/**
 * @brief 设置是否启用强制 ACL 检查
 * @param reg 注册表指针
 * @param enforce true 仅允许通过 ACL 匹配的操作；false 使用 target 自身的默认权限
 */
void ecat_target_registry_set_enforce_acl(ecat_target_registry_t *reg, bool enforce);

/**
 * @brief 权限校验：判断指定传输通道是否对指定目标拥有对应权限
 * @param reg 注册表指针
 * @param transport_id 传输通道 ID
 * @param target_id 目标 ID
 * @param required_perm 要求的操作权限 (如 ECAT_PERM_READ 或 ECAT_PERM_WRITE)
 * @return true 允许访问，false 拒绝访问
 */
bool ecat_target_registry_check_permission(
    const ecat_target_registry_t *reg,
    uint16_t transport_id,
    uint16_t target_id,
    uint8_t required_perm);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_TARGET_REGISTRY_H */
