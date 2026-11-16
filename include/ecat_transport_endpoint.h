/**
 * @file ecat_transport_endpoint.h
 * @brief 通用传输端点抽象与自定义传输通道适配层 (Transport Endpoint Interface)
 *
 * 核心目标:
 * 1. 任意传输通道可插拔: 内置通道 (FoE, File 等) 可选开启或完全裁剪 (0 Flash 占用)。
 * 2. 极简自定义通道: 用户可基于本接口轻量接入自定义传输 (CAN, UART, 485, BLE, 私有协议等)。
 * 3. 统一会话绑定: 将底层物理协议接收/发送与双向传输管理器 (ecat_transfer_t) 安全对接。
 * 4. 零动态分配 (Zero malloc): 静态描述符与非侵入式句柄。
 */

#ifndef ECAT_TRANSPORT_ENDPOINT_H
#define ECAT_TRANSPORT_ENDPOINT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ecat_transfer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ECAT_TRANSPORT_NAME_MAX_LEN 16U

/* 预留知名传输通道 ID 编号 */
#define ECAT_TRANSPORT_ID_GENERIC 0U
#define ECAT_TRANSPORT_ID_FOE     1U
#define ECAT_TRANSPORT_ID_FILE    2U
#define ECAT_TRANSPORT_ID_UART    3U
#define ECAT_TRANSPORT_ID_CAN     4U
#define ECAT_TRANSPORT_ID_USER_BASE 100U

/* 传输端点状态 */
typedef enum {
    ECAT_TRANSPORT_STATE_IDLE = 0,
    ECAT_TRANSPORT_STATE_IMPORTING,
    ECAT_TRANSPORT_STATE_EXPORTING,
    ECAT_TRANSPORT_STATE_ERROR
} ecat_transport_state_t;

/* 自定义传输通道事件回调 (可选，用于协议栈事件通知) */
typedef struct {
    /** 传输会话启动事件通知 (方向与目标 ID) */
    void (*on_session_start)(void *user_ctx, ecat_transfer_dir_t dir, uint16_t target_id);
    /** 传输进度事件通知 (当前已传输字节数) */
    void (*on_progress)(void *user_ctx, uint32_t transferred_bytes, uint32_t total_bytes);
    /** 传输会话成功完成通知 */
    void (*on_session_complete)(void *user_ctx, uint32_t total_transferred, uint32_t crc32);
    /** 传输会话异常中止通知 */
    void (*on_session_abort)(void *user_ctx, ecat_transfer_err_t err);
} ecat_transport_callbacks_t;

/* 传输端点实例结构体 */
typedef struct {
    uint16_t transport_id;                              /* 通道唯一 ID */
    char name[ECAT_TRANSPORT_NAME_MAX_LEN];             /* 通道描述名称 */
    ecat_transfer_t *transfer_mgr;                      /* 绑定的双向传输管理器 */
    uint64_t current_token;                             /* 当前持有会话的令牌 */
    ecat_transport_state_t state;                       /* 端点内部运行状态 */
    uint32_t total_size;                                /* 当前会话预估或实际总量 */
    ecat_transport_callbacks_t cbs;                     /* 可选状态通知回调 */
    void *user_ctx;                                     /* 用户自定义上下文 */
    bool finishing;                                     /* 是否处于异步终结提交阶段 */
    bool begin_pending;                                 /* 是否处于异步会话启动就绪阶段 */
    uint16_t pending_target_id;                         /* 挂起启动时的目标 ID */
} ecat_transport_endpoint_t;

/* -------------------------------------------------------------------------- */
/*                                API 接口声明                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化一个传输端点 (用于内置通道或用户自定义通道)
 * @param ep 端点句柄指针
 * @param transport_id 通道唯一标识符
 * @param name 通道名称
 * @param mgr 双向传输管理器指针
 * @param user_ctx 用户上下文
 * @return 0 成功，-1 参数非法
 */
int ecat_transport_endpoint_init(
    ecat_transport_endpoint_t *ep,
    uint16_t transport_id,
    const char *name,
    ecat_transfer_t *mgr,
    void *user_ctx);

/**
 * @brief 注册可选的事件通知回调 (进度、完成、中止等)
 */
void ecat_transport_endpoint_set_callbacks(
    ecat_transport_endpoint_t *ep,
    const ecat_transport_callbacks_t *cbs);

/**
 * @brief 发起数据导入会话 (向设备目标区块写入/升级)
 *
 * 【并发与异步生命周期契约】：
 * - 若底层驱动为同步或立即就绪，返回 ECAT_TRANSFER_COMPLETE，并触发 on_session_start 回调；
 * - 若底层为异步耗时初始化，返回 ECAT_TRANSFER_PENDING，此时会话处于 pending 状态，必须通过
 *   ecat_transport_endpoint_poll 推进，就绪时单次触发 on_session_start 并返回 ECAT_TRANSFER_COMPLETE；
 * - 在 pending 期间调用 write_chunk / finish_import 将被返回 ECAT_TRANSFER_BUSY 拦截保护；
 * - 若管理器已被其他通道占用返回 ECAT_TRANSFER_BUSY；权限不足返回 ECAT_TRANSFER_DENIED。
 *
 * @param ep 端点句柄
 * @param target_id 目标区块 ID
 * @param expected_size 预期写入总字节数 (0 表示按流式结束)
 * @param fw_version 固件版本号 (固件类目标有效)
 * @return 传输结果码 (ECAT_TRANSFER_COMPLETE, ECAT_TRANSFER_PENDING, ECAT_TRANSFER_BUSY 等)
 */
ecat_transfer_result_t ecat_transport_endpoint_begin_import(
    ecat_transport_endpoint_t *ep,
    uint16_t target_id,
    uint32_t expected_size,
    uint32_t fw_version);

/**
 * @brief 分块推送导入数据流
 *
 * 【异步 I/O 与并发保护】：
 * - 异步驱动下可能返回 ECAT_TRANSFER_PENDING，需通过 ecat_transport_endpoint_poll 轮询推进；
 * - 前序操作未完成时重复调用返回 ECAT_TRANSFER_BUSY 拦截。
 *
 * @param ep 端点句柄
 * @param data 接收到的数据块指针
 * @param length 数据块长度
 * @return ECAT_TRANSFER_COMPLETE 成功，ECAT_TRANSFER_PENDING 异步中，ECAT_TRANSFER_BUSY 正在忙，其他为错误码
 */
ecat_transfer_result_t ecat_transport_endpoint_write_chunk(
    ecat_transport_endpoint_t *ep,
    const uint8_t *data,
    size_t length);

/**
 * @brief 完成并提交导入会话
 *
 * 【终结与异步完成契约】：
 * - 提交成功且同步完成时返回 ECAT_TRANSFER_COMPLETE，触发 on_session_complete 并恢复 IDLE 态；
 * - 异步 finalize 驱动下返回 ECAT_TRANSFER_PENDING，需通过 poll 推进，最终单次触发 on_session_complete。
 *
 * @param ep 端点句柄
 * @return ECAT_TRANSFER_COMPLETE 提交成功，ECAT_TRANSFER_PENDING 异步提交中，ECAT_TRANSFER_BUSY 忙
 */
ecat_transfer_result_t ecat_transport_endpoint_finish_import(
    ecat_transport_endpoint_t *ep);

/**
 * @brief 发起数据导出回读会话 (从设备目标区块导出数据)
 *
 * 【并发与导出总大小确定性契约】：
 * - 可导出总大小（*out_total_size）的确定：
 *   - 无论是同步还是异步模式，传输引擎在底层启动前已根据目标驱动的 get_data_size()、
 *     请求参数的 expected_size 或注册表 target->max_size 完成计算；
 *   - 只要 out_total_size 指针非空，本函数在同步就绪（ECAT_TRANSFER_COMPLETE）或
 *     异步打开挂起（ECAT_TRANSFER_PENDING）时均已将该总大小写入 *out_total_size；
 *   - 后续调用 poll 时不会保存也不会回填该指针；调用方在 begin_export 返回时即可立即获取并使用该数值；
 * - 异步只读打开（READ_OPEN）挂起时返回 ECAT_TRANSFER_PENDING，需通过 poll 推进直到
 *   ECAT_TRANSFER_COMPLETE 后单次派发 on_session_start 回调；
 * - 在 pending 期间调用 read_chunk / finish_export 返回 ECAT_TRANSFER_BUSY 拦截。
 *
 * @param ep 端点句柄
 * @param target_id 目标区块 ID
 * @param out_total_size 传出实际可导出的总字节数 (本函数返回 COMPLETE 或 PENDING 时均已写入)
 * @return ECAT_TRANSFER_COMPLETE 成功建立导出通道，ECAT_TRANSFER_PENDING 异步就绪中，其他为错误/占用
 */
ecat_transfer_result_t ecat_transport_endpoint_begin_export(
    ecat_transport_endpoint_t *ep,
    uint16_t target_id,
    uint32_t *out_total_size);

/**
 * @brief 分块拉取导出数据
 *
 * 【异步 I/O 与指针持有生命周期契约 (Strict Lifetime Contract)】：
 * - 异步驱动模式下，若底层驱动返回 PENDING，本函数返回 ECAT_TRANSFER_PENDING；
 * - **指针生命周期与有效性要求**：调用方传入的 out_buf 缓冲区和 actual_read 指针在返回
 *   ECAT_TRANSFER_PENDING 后，会被底层传输引擎持续持有并记录于内部上下文。调用方必须保证：
 *   1) out_buf 所指向的内存在整个 pending 期间保持有效、未被释放、未被并发修改或移动；
 *   2) actual_read 所指向的指针变量在整个 pending 期间必须持续存活且不可重用；
 *   3) 本函数在调用底层异步驱动前，会明确将 *actual_read 预先清零 (*actual_read = 0U)。
 *      在异步挂起 (PENDING) 期间，实际读取数据尚未就绪，调用方绝不得依赖 *actual_read 的临时数值；
 *   4) 直到后续由调用方通过 ecat_transport_endpoint_poll 轮询推进并返回 ECAT_TRANSFER_COMPLETE
 *      或 CONTINUE 时，底层引擎才将实际读取字节数正式写入 *actual_read 并触发 on_progress 回调；
 *   5) 若调用 ecat_transport_endpoint_abort 中止会话，契约要求底层设备驱动的 abort 接口必须同步停止
 *      硬件 DMA/外设传输；传输引擎在 abort 流程中重置 pending 标记与内部阶段，保证后续绝不再使用
 *      这两个外部指针，调用方随后方可安全释放或复用缓冲区；
 * - 同步模式或当次立即完成时：若数据已读完返回 ECAT_TRANSFER_COMPLETE，若仍有后续数据返回
 *   ECAT_TRANSFER_CONTINUE，*actual_read 被立即写入；
 * - 若会话处于前序 PENDING 状态时重复调用，返回 ECAT_TRANSFER_BUSY；
 * - 会话未激活返回 ECAT_TRANSFER_INVALID；驱动错误返回 ECAT_TRANSFER_FAILED (并派发 abort 通知)。
 *
 * @param ep 端点句柄
 * @param out_buf 输出缓冲区 (异步 pending 期间必须持续存活)
 * @param buf_capacity 缓冲区容量
 * @param actual_read 传出本次实际读取的字节数 (发起时置 0，异步完成时由 poll 写入，pending 期间必须持续存活)
 * @return ECAT_TRANSFER_COMPLETE 数据已全部读取完毕;
 *         ECAT_TRANSFER_CONTINUE 成功读取当前块，后续仍有数据;
 *         ECAT_TRANSFER_PENDING 异步读取已在后台发起，指针被引擎持有，需通过 poll 推进;
 *         ECAT_TRANSFER_BUSY 当前仍有未完成的异步操作，不可并发重入;
 *         其他为错误码 (同时触发 abort 事件通知并清理会话)
 */
ecat_transfer_result_t ecat_transport_endpoint_read_chunk(
    ecat_transport_endpoint_t *ep,
    uint8_t *out_buf,
    size_t buf_capacity,
    size_t *actual_read);

/**
 * @brief 完成导出回读会话
 * @param ep 端点句柄
 * @return ECAT_TRANSFER_COMPLETE 导出完成，ECAT_TRANSFER_PENDING 异步关闭中
 */
ecat_transfer_result_t ecat_transport_endpoint_finish_export(
    ecat_transport_endpoint_t *ep);

/**
 * @brief 中止或取消当前端点的活动会话
 *
 * 【硬件停止与资源回收契约】：
 * - 依赖前提：底层设备驱动的 abort 实现必须在回调中同步停止底层硬件 DMA/外设传输；
 * - 端点中止流程将安全重置 pending 状态与 stage，释放底层传输会话锁并恢复 IDLE 态，
 *   保证后续绝不再解引用或写入异步 read_chunk 时传入的 out_buf 和 actual_read 指针；
 * - 触发 on_session_abort 回调通知上层应用。
 *
 * @param ep 端点句柄
 */
void ecat_transport_endpoint_abort(ecat_transport_endpoint_t *ep);

/**
 * @brief 判断当前端点是否持有激活会话
 */
bool ecat_transport_endpoint_is_active(const ecat_transport_endpoint_t *ep);

/**
 * @brief 判断当前端点是否有未完成的底层异步 I/O 正在进行
 * @param ep 端点句柄
 * @return true 正在等待异步驱动推进，false 无异步挂起
 */
bool ecat_transport_endpoint_is_pending(const ecat_transport_endpoint_t *ep);

/**
 * @brief 轮询并推进端点底层异步操作
 *
 * 【空闲与错误处理契约】：
 * - 若端点未处于激活状态，返回 ECAT_TRANSFER_INVALID；
 * - **空闲轮询契约**：若端点持有活动会话但当前无正在进行中的异步挂起操作 (!is_pending)，
 *   本函数立即返回 ECAT_TRANSFER_INVALID，**严格保留**当前活动会话、令牌与状态，
 *   **绝不触发** on_session_abort 回调，允许调用方在非阻塞主循环中安全持续轮询；
 * - 若底层异步驱动仍在处理中，返回 ECAT_TRANSFER_PENDING；
 * - 若异步操作完成，根据阶段分别触发 on_session_start、on_progress 或 on_session_complete；
 * - 仅当底层驱动在 pending 推进过程中发生真正错误时，才调用 cancel 中止会话，
 *   触发 on_session_abort 并将状态置为 ERROR。
 *
 * @param ep 端点句柄
 * @return ECAT_TRANSFER_PENDING 驱动仍在处理;
 *         ECAT_TRANSFER_COMPLETE 异步阶段或终结操作完成;
 *         ECAT_TRANSFER_CONTINUE 异步读取阶段完成但后续仍有数据;
 *         ECAT_TRANSFER_INVALID 无活动会话或当前处于空闲无挂起操作;
 *         其他为错误码 (同时触发 abort 事件通知并清理会话)
 */
ecat_transfer_result_t ecat_transport_endpoint_poll(ecat_transport_endpoint_t *ep);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_TRANSPORT_ENDPOINT_H */
