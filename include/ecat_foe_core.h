/**
 * @file ecat_foe_core.h
 * @brief EtherCAT FoE 核心升级状态机接口定义 (依据 SAD-v0.1 第 1、3、4 节)
 *
 * 核心状态机特性：
 * - 纯 C99 实现，零 malloc，零动态内存分配
 * - 严格同步阻塞执行模型
 * - 强防变砖保证：先 invalidate，再按需 erase_to，双重 CRC 比对，成功调 finalize，失败调 abort
 * - 细分内部错误枚举，支持映射到标准 EtherCAT FoE 错误码 (含 SOES 数据阶段 0x800B 收敛)
 */

#ifndef ECAT_FOE_CORE_H
#define ECAT_FOE_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ecat_foe_efw.h"
#include "ecat_foe_target.h"
#include "ecat_foe_conf_template.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                               常量与宏定义                                 */
/* -------------------------------------------------------------------------- */
#define ECAT_FOE_MAX_FILENAME_LEN           31U  /**< FoE 请求文件名最大长度 (字节) */

/* -------------------------------------------------------------------------- */
/*                               FoE 标准错误码                               */
/* -------------------------------------------------------------------------- */
#define ECAT_FOE_ERRCODE_NOTDEFINED         0x8000U
#define ECAT_FOE_ERRCODE_NOTFOUND           0x8001U  /* 文件/目标不存在或未使能 */
#define ECAT_FOE_ERRCODE_ACCESS             0x8002U  /* 工程/硬件ID不匹配、版本超区间 */
#define ECAT_FOE_ERRCODE_DISKFULL           0x8003U  /* 超容量、多收溢出熔断 */
#define ECAT_FOE_ERRCODE_ILLEGAL            0x8004U  /* 魔数/版本错、非零flags、防回滚 */
#define ECAT_FOE_ERRCODE_PACKETNO           0x8005U
#define ECAT_FOE_ERRCODE_ALREADYEXISTS      0x8006U
#define ECAT_FOE_ERRCODE_PROGERROR          0x800BU  /* 编程失败 (SOES 数据阶段固定收敛) */
#define ECAT_FOE_ERRCODE_CHECKSUM           0x800CU  /* 头部/流/回读 CRC 不匹配 */

/* -------------------------------------------------------------------------- */
/*                           核心内部细分诊断错误码                           */
/* -------------------------------------------------------------------------- */
typedef enum {
    ECAT_FOE_CORE_OK = 0,

    /* 0. 核心配置与初始化错误 */
    ECAT_FOE_CORE_ERR_CFG_INVALID,              /**< 配置参数指针为空或配置非法 */
    ECAT_FOE_CORE_ERR_CFG_PROJECT_NAME,         /**< project_name 为空或长度超出限制 */
    ECAT_FOE_CORE_ERR_CFG_HEADER_BUF,           /**< 头部缓冲配置非法 (<64 或非 4 字节对齐) */
    ECAT_FOE_CORE_ERR_CFG_TARGET,               /**< 启用目标驱动或其回调为空，或容量/对齐非法 */

    /* 1. WRQ 阶段错误 */
    ECAT_FOE_CORE_ERR_WRQ_INVALID_NAME,        /**< 文件名语法错误或无法切分 */
    ECAT_FOE_CORE_ERR_WRQ_PROJECT_MISMATCH,    /**< 文件名项目字符串与本地配置不符 */
    ECAT_FOE_CORE_ERR_WRQ_TARGET_UNKNOWN,      /**< 目标字样未知 */
    ECAT_FOE_CORE_ERR_WRQ_TARGET_DISABLED,     /**< 目标在本地配置中被禁用 */

    /* 1.1 防回滚错误 */
    ECAT_FOE_CORE_ERR_ANTI_ROLLBACK_GET,       /**< 获取版本下限失败拒绝升级 */
    ECAT_FOE_CORE_ERR_ANTI_ROLLBACK_UPDATE,    /**< finalize 成功后更新版本下限失败 (固件保持有效) */

    /* 2. 头部装配与核验错误 */
    ECAT_FOE_CORE_ERR_HDR_PROBE,               /**< 阶段 1 探测失败 (魔数/版本/长度) */
    ECAT_FOE_CORE_ERR_HDR_CRC,                 /**< 阶段 2 Header CRC32 校验失败 */
    ECAT_FOE_CORE_ERR_HDR_VERIFY,              /**< 阶段 3 全量元数据安全核验失败 */
    ECAT_FOE_CORE_ERR_HDR_CROSS_MISMATCH,      /**< 文件名与文件头交叉比对失败 (target/version) */
    ECAT_FOE_CORE_ERR_PAYLOAD_LIMIT_EXCEEDED,  /**< payload_length 超过本地分区容量上限 */

    /* 3. 目标驱动调用错误 */
    ECAT_FOE_CORE_ERR_DRV_NOT_REGISTERED,      /**< 目标驱动未注册 */
    ECAT_FOE_CORE_ERR_DRV_INIT,                /**< drv->init() 失败 */
    ECAT_FOE_CORE_ERR_DRV_INVALIDATE,          /**< drv->invalidate() 失败 */
    ECAT_FOE_CORE_ERR_DRV_ERASE,               /**< drv->erase_to() 失败 */
    ECAT_FOE_CORE_ERR_DRV_WRITE,               /**< drv->write() 失败 */
    ECAT_FOE_CORE_ERR_DRV_READ,                /**< drv->read() 失败 */
    ECAT_FOE_CORE_ERR_DRV_FINALIZE,            /**< drv->finalize() 失败 */

    /* 4. 流接收与校验错误 */
    ECAT_FOE_CORE_ERR_OVER_RECEIVE,            /**< 累计偏移超过期望总长，即时熔断 */
    ECAT_FOE_CORE_ERR_UNDER_RECEIVE,           /**< 结束时实际接收总长少于期望总长 */
    ECAT_FOE_CORE_ERR_STREAM_CRC_MISMATCH,     /**< 数据流 CRC32 与文件头不符 */
    ECAT_FOE_CORE_ERR_READBACK_CRC_MISMATCH,   /**< 物理介质回读 CRC32 与文件头不符 */

    /* 5. 协议与状态机错误 */
    ECAT_FOE_CORE_ERR_INVALID_STATE,           /**< 状态机状态异常 */
    ECAT_FOE_CORE_ERR_INVALID_ARG              /**< 入参非法 */
} ecat_foe_core_err_t;

/* -------------------------------------------------------------------------- */
/*                               核心状态机状态                               */
/* -------------------------------------------------------------------------- */
typedef enum {
    ECAT_FOE_STATE_IDLE = 0,                   /**< 空闲状态，等待 WRQ */
    ECAT_FOE_STATE_WRQ_ACKED,                  /**< WRQ 通过，等待数据流并装配头部 */
    ECAT_FOE_STATE_DATA,                       /**< 头部校验全过，进入固件擦写阶段 */
    ECAT_FOE_STATE_SUCCESS,                    /**< 升级全流程成功，等待复位 */
    ECAT_FOE_STATE_ERROR                       /**< 发生错误，驱动已中止清理 */
} ecat_foe_state_t;

/* -------------------------------------------------------------------------- */
/*                             核心配置与注册结构                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief 目标驱动注册描述符
 */
typedef struct {
    uint16_t target_id;                        /**< 目标 ID (1..4) */
    bool     enabled;                          /**< 是否启用 */
    uint32_t max_payload_size;                 /**< 分区最大允许容量 (字节) */
    uint32_t write_unit;                       /**< 驱动物理写入对齐单位 (如 8, 256) */
    const ecat_foe_target_driver_t *driver;    /**< 驱动操作集 */
    void    *user_ctx;                         /**< 驱动自定义上下文 */
} ecat_foe_target_desc_t;

/**
 * @brief 核心机全局静态配置
 */
typedef struct {
    uint32_t project_id;                       /**< 本地工程 ID (0x10010001) */
    /**
     * @brief 本地工程名 (如 "SERVO_DRV")
     *
     * 【内存与生命周期契约】：
     * - 调用者传入的 project_name 必须是以 '\0' 结尾的合法字符串，长度在 [1, ECAT_FOE_MAX_FILENAME_LEN] (即 1..31 字节)；
     * - 超长 (超过 31 字节) 坚决报错拒绝初始化 (返回 -1)，绝不进行任何静默截断；
     * - ecat_foe_core_init 会在校验通过后将其完整深拷贝至 core->local_project_name 静态数组中；
     * - 函数返回后，调用者传入的字符串内存可立即释放或被覆写（如栈局部变量），核心内部唯一信赖 local_project_name。
     */
    const char *project_name;
    uint32_t hardware_id;                      /**< 本地硬件 ID (0x00000001) */
    uint16_t local_hw_ver;                     /**< 本地硬件版本号 (uint16) */
    bool     anti_rollback_enable;             /**< 是否启用防回滚 */
    uint16_t max_header_size;                  /**< 允许的头部缓冲上限 (至少 64 字节且 4 字节对齐) */
    ecat_foe_target_desc_t targets[ECAT_FOE_TARGET_COUNT]; /**< 目标 1..4 */
} ecat_foe_core_cfg_t;

/**
 * @brief 核心机运行上下文结构体 (纯静态内存占用)
 *
 * 【内存布局与复制契约 (Lifecycle & Copy Contract)】：
 * - 内部项目名通过 local_project_name 静态数组持久化存储，消除了结构体内部的自身指针自引用；
 * - 【非活跃状态 (IDLE) 浅拷贝限制】：
 *   在尚未开启升级会话的状态下，结构体不含悬垂自指针，支持按值复制/赋值；
 *   【重要警告】：浅拷贝仅复制标量配置，targets[i].driver 与 targets[i].user_ctx 仍为共享指针！
 *   若多实例浅拷贝自同一模板，除非为各实例显式替换独立的 user_ctx 上下文，否则严禁并发操作！
 * - 【活动会话状态 (WRQ_ACKED / DATA / 升级进行中)】：
 *   严禁浅拷贝或移动！由于底层驱动会话状态（物理扇区擦除水位、会话独占锁、增量 CRC）具有硬件唯一性，
 *   复制活动会话会导致多实例状态撕裂与物理硬件踩踏破坏。
 */
typedef struct {
    ecat_foe_core_cfg_t cfg;                   /**< 本地静态配置拷贝 */
    bool                is_configured;         /**< 核心是否已通过合规静态配置校验 */
    ecat_foe_state_t    state;                 /**< 当前状态机状态 */
    ecat_foe_core_err_t last_error;            /**< 最近一次细分错误码 */

    /* 会话生命周期与驱动调用保护标记 */
    uint32_t            target_incomplete_mask; /**< 按目标跟踪的未闭环位掩码 (bit T-1 表示目标 T 驱动已 init 但未成功 finalize) */
    bool                upgrade_incomplete;    /**< 固件升级未闭环标志 (存在任一未完成目标为 true) */
    bool                session_active;        /**< 会话是否已激活 (已调用过 init/invalidate) */
    bool                driver_abort_called;   /**< 底层驱动 abort() 是否已被调用过 (防重复 abort) */
    bool                driver_finalized;      /**< 底层驱动 finalize() 是否已成功执行 (防 finalize 后 abort) */
    bool                driver_invalidated;    /**< 底层驱动 invalidate() 是否已成功执行 (固件有效标记已失效) */

    /* 本地安全持久化拷贝 (解除对外部配置指针生命周期的依赖) */
    char                local_project_name[ECAT_FOE_MAX_FILENAME_LEN + 1];

    /* WRQ 解析缓存 */
    uint16_t            req_target_id;         /**< 从文件名解析出的 target_id */
    uint32_t            req_fw_version;        /**< 从文件名解析出的 fw_version */

    /* 头部装配缓存 */
    uint8_t             header_buf[ECAT_FOE_HEADER_BUF_MAX];
    uint16_t            header_rx_len;         /**< 已接收头部字节数 */
    uint16_t            header_total_len;      /**< 头部标称总长度 (>=64) */
    bool                header_complete;       /**< 头部装配校验是否已闭环 */
    ecat_foe_efw_header_t hdr;                 /**< 解析装配后的头部元数据 */

    /* 当前会话绑定的驱动指针 */
    const ecat_foe_target_driver_t *active_drv;
    void               *active_user_ctx;
    uint32_t            active_write_unit;     /**< 当前目标的 write_unit */

    /* 数据流与擦写状态 */
    uint64_t            total_rx_bytes;        /**< 累计接收总字节数 (含头部) */
    uint32_t            payload_rx_bytes;      /**< 累计接收有效负载字节数 */
    uint32_t            erased_watermark;      /**< 当前物理介质已擦除安全水位 */
    uint32_t            written_offset;        /**< 物理介质已对齐写入偏移 */
    uint32_t            stream_crc;            /**< 有效 Payload 增量流 CRC32 */

    /* 残留对齐缓冲 (凑满 write_unit 再下发驱动写入) */
    uint8_t             residual_buf[ECAT_FOE_MAX_WRITE_UNIT];
    uint32_t            residual_len;
} ecat_foe_core_t;

/* -------------------------------------------------------------------------- */
/*                               核心机外部接口                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化并校验核心机静态配置
 *
 * 【内存、校验与生命周期契约】：
 * - 纯 C99 实现，零动态内存分配 (malloc)；
 * - 校验通过前严禁读取未初始化的 core（如 is_configured），防止未初始化脏内存引发 UBSan 非法 bool 读取未定义行为；
 * - cfg->project_name 为 NULL 或非法时一律返回 -1 拒绝，失败后 core 重置并标记为不可用 (is_configured == false)；
 * - 校验通过后将 cfg->project_name 深拷贝到内部 local_project_name 静态数组，同时将 core->cfg.project_name 置为 NULL（消除内部自身指针与悬垂引用）；
 * - 【重要契约】：因 core->cfg.project_name 为 NULL，不支持直接传入 &core->cfg 重新初始化；重新初始化必须传入包含非空 project_name 的独立合法配置；
 * - 升级会话异常中断后，由 ecat_foe_core_on_wrq 入口执行会话级精细自愈，无需外部重复调用 init。
 *
 * @param core 核心机上下文指针
 * @param cfg  静态配置指针
 * @return 0 成功；非 0 失败 (配置参数不合规，核心机标记不可用)
 */
int ecat_foe_core_init(ecat_foe_core_t *core, const ecat_foe_core_cfg_t *cfg);

/**
 * @brief 反初始化核心升级状态机并安全释放活跃会话底层硬件资源
 *
 * 【调用契约与入参安全边界 (Lifecycle & Safety Contract)】：
 * - 仅接受以下两类合法入参指针：
 *   1. NULL (安全无操作，直接返回 0)；
 *   2. 经 ecat_foe_core_init() 成功初始化的有效 core 实例；或已显式执行零初始化（如 ecat_foe_core_t core = {0} 或 memset(&core, 0, sizeof(core))）的实例。
 * - 【严禁传入未显式初始化的脏栈内存】：
 *   本函数绝不可被声称亦不支持直接在未初始化的脏栈内存上调用。在 C 语言规范及 UBSan 检查下，
 *   读取包含未初始化垃圾数据的布尔字段（如 !core->is_configured）属于未定义行为 (UB)。
 *
 * 【资源释放与状态机重置】：
 * - 若 core 当前处于活跃会话中 (core->session_active 为 true)：
 *   若底层驱动已初始化且尚未调用 abort 或 finalize，显式调用目标驱动的 abort 回调释放底层硬件/闪存资源；
 *   标记 driver_abort_called 为 true。
 * - 清空 core 全部内存 (memset 为 0)，显式将 is_configured 置为 false，状态重置为 ECAT_FOE_STATE_IDLE。
 * - 【重新初始化规范】：ecat_foe_core_init() 严禁前置读取未初始化 core 的任何字段以防未定义行为；若需要
 *   重新初始化一个处于活跃会话的 core 实例，调用方必须先显式调用 ecat_foe_core_deinit(core)，然后再执行 ecat_foe_core_init()。
 *
 * @param core 核心状态机实例指针 (必须为 NULL、有效已初始化或经显式零初始化的实例)
 * @return 0 成功
 */
int ecat_foe_core_deinit(ecat_foe_core_t *core);

/**
 * @brief 查询当前核心是否存在未闭环的固件升级 (任何启用的目标驱动已 init 但尚未由相同目标成功 finalize)
 *
 * @note 局限性说明：此状态仅在当前 core 实例的单次内存生命周期内由位掩码跟踪；
 *       若发生 deinit/reinit 或硬件断电重启，RAM 内存数据清零，必须依赖底层非易失性介质
 *       的物理有效性检查与 Bootloader 启动引导校验兜底。
 *
 * @param core 核心机指针
 * @return true 存在任一目标处于升级未闭环状态
 * @return false 未初始化或所有已启动目标均已闭环
 */
bool ecat_foe_core_upgrade_incomplete(const ecat_foe_core_t *core);

/**
 * @brief 查询特定目标当前是否存在未闭环的固件升级
 *
 * @param core 核心机指针
 * @param target_id 目标编号 (1 ~ ECAT_FOE_TARGET_COUNT)
 * @return true 该目标处于升级未闭环状态
 * @return false 该目标未启动升级或已成功 finalize 闭环
 */
bool ecat_foe_core_target_upgrade_incomplete(const ecat_foe_core_t *core, uint16_t target_id);

/**
 * @brief 处理 FoE WRQ 写请求 (文件名解析与合法性快速判定)
 * 若上一次会话未闭环，主动调驱动 abort 并重置会话。
 * @param core         核心机上下文指针
 * @param filename     主站请求写入的文件名缓冲区
 * @param filename_len 文件名长度 (字节，必须 <= FOE_FN_MAX)
 * @return 0 成功；非 0 返回标准 FoE 错误码 (0x8001, 0x8002, 0x8004 等)
 */
int ecat_foe_core_on_wrq(ecat_foe_core_t *core, const char *filename, size_t filename_len);

/**
 * @brief 处理 FoE 数据块写入
 * 包含头部流式装配、擦除前全量核验、按需覆盖擦除、write_unit 残留缓冲对齐写入与即时多收熔断。
 * @param core 核心机上下文指针
 * @param data 接收到的数据块指针
 * @param len  数据块长度 (字节，len == 0 视为传输结束)
 * @return 0 成功；非 0 失败 (SOES 适配层收敛回复 0x800B)
 */
int ecat_foe_core_on_data(ecat_foe_core_t *core, const uint8_t *data, size_t len);

/**
 * @brief 处理传输结束通知
 * 补齐残留 0xFF 写入、比对流 CRC、物理介质分块回读 CRC 比对并调用 finalize()/abort()。
 * @param core 核心机上下文指针
 * @return 0 成功；非 0 失败
 */
int ecat_foe_core_on_end(ecat_foe_core_t *core);

/**
 * @brief 异常中止通知 (主站取消或网络异常)
 * 释放底层驱动并重置核心状态机。
 */
void ecat_foe_core_on_abort(ecat_foe_core_t *core);

/**
 * @brief 获取核心机最近一次细分错误码
 */
ecat_foe_core_err_t ecat_foe_core_get_last_error(const ecat_foe_core_t *core);

/**
 * @brief 将核心内部错误枚举映射为标准 FoE 错误码
 * @param err           核心内部细分错误
 * @param is_wrq_stage  是否为 WRQ 阶段 (true 时透传细分码；false 时 SOES 数据阶段收敛为 0x800B)
 * @param soes_mode     是否遵循 SOES 限制 (true 时数据阶段一律回 0x800B)
 */
uint16_t ecat_foe_core_err_to_foe(ecat_foe_core_err_t err, bool is_wrq_stage, bool soes_mode);

/**
 * @brief 错误码转换为人类可读描述字符串
 */
const char *ecat_foe_core_err_to_str(ecat_foe_core_err_t err);

/**
 * @brief 获取核心机内部持久化存储的项目名
 * @param core 核心机上下文指针
 * @return 指向内部持久化以 '\0' 结尾项目名字符串的指针，若 core 为空返回 NULL
 */
const char *ecat_foe_core_get_project_name(const ecat_foe_core_t *core);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_FOE_CORE_H */
