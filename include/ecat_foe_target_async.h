/**
 * @file ecat_foe_target_async.h
 * @brief Allocation-free single-operation async target-driver runner.
 *
 * This is the non-blocking target-operation primitive used by the async core
 * integration. It is additive: the existing synchronous target-driver API is
 * unchanged. The runner owns one fixed-size staging buffer and never queues a
 * second operation while one is pending.
 */
#ifndef ECAT_FOE_TARGET_ASYNC_H
#define ECAT_FOE_TARGET_ASYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ECAT_FOE_TARGET_ASYNC_FAILED = -1,
    ECAT_FOE_TARGET_ASYNC_COMPLETE = 0,
    ECAT_FOE_TARGET_ASYNC_PENDING = 1
} ecat_foe_target_async_result_t;

typedef enum {
    ECAT_FOE_TARGET_OP_INIT = 0,
    ECAT_FOE_TARGET_OP_INVALIDATE,
    ECAT_FOE_TARGET_OP_ERASE_TO,
    ECAT_FOE_TARGET_OP_WRITE,
    ECAT_FOE_TARGET_OP_READ,
    ECAT_FOE_TARGET_OP_FINALIZE
} ecat_foe_target_op_kind_t;

/* The descriptor and its data remain stable from start until completion. */
typedef struct {
    ecat_foe_target_op_kind_t kind;
    uint32_t offset;
    uint32_t need_end;
    uint32_t erased_end; /* Driver writes this for ERASE_TO. */
    uint32_t length;
    uint8_t *data;       /* WRITE input or READ output staging buffer. */
    uint16_t target_id;  /* FINALIZE metadata; ignored for other operations. */
    uint32_t fw_version;
    uint32_t min_allow_version;
    bool anti_rollback;
} ecat_foe_target_async_op_t;

typedef struct ecat_foe_target_async_ops {
    ecat_foe_target_async_result_t (*start)(void *ctx,
                                            ecat_foe_target_async_op_t *op);
    ecat_foe_target_async_result_t (*poll)(void *ctx,
                                           ecat_foe_target_async_op_t *op);
    /**
     * @brief 取消当前正在 pending 执行中的单步驱动操作 (非阻塞同步停止硬件)
     */
    void (*abort)(void *ctx);
    /**
     * @brief 释放活动目标驱动会话的所有会话级资源
     *
     * 触发契约与兼容性设计:
     * 1. 默认/旧版驱动契约 (未声明 async_safe_session_abort):
     *    若 ECAT_FOE_TARGET_OP_INIT 的 start() 立即返回 FAILED，遵循与同步驱动相同的契约，
     *    由驱动自身负责在 start() 内部回滚释放部分获取的资源，框架不会调用 abort_session()。
     *    这确保了旧版在 abort_session() 中包含介质失效逻辑的驱动不会误破坏既有有效固件镜像。
     * 2. 新版安全契约 (显式声明 async_safe_session_abort):
     *    若驱动显式声明该能力，在 start(INIT) 立即失败时框架将安全调用 abort_session() 统一
     *    回收驱动资源；驱动的 abort_session() 必须保证不破坏尚未执行 INVALIDATE 的旧有效镜像。
     * 3. PENDING 期间失败与取消:
     *    若 INIT 操作进入 PENDING 状态，会话已正式确立。后续若 poll(INIT) 返回 FAILED 或外部取消，
     *    框架将调用 ops->abort (停止硬件) 并调用 ops->abort_session() 释放会话资源。
     * 4. 边界约束 (Legacy Driver Inherent Boundary):
     *    若旧版驱动在 abort_session() 中无条件清除了有效镜像标记 (valid=false)，一旦其 INIT
     *    进入 PENDING 并在后续失败或取消，框架因必须中止硬件而调用 abort_session()，此时旧驱动
     *    自身将清除 valid 标记。驱动若需在任何失败时机均保护旧镜像，必须遵循将介质失效严格限定于
     *    ECAT_FOE_TARGET_OP_INVALIDATE 操作的新规范。
     * 5. 成功闭环: 若 ECAT_FOE_TARGET_OP_FINALIZE 成功完成，会话正常提交结束，后续不再调用 abort_session()。
     */
    void (*abort_session)(void *ctx);
} ecat_foe_target_async_ops_t;

typedef struct {
    const ecat_foe_target_async_ops_t *ops;
    void *ctx;
    uint8_t *storage;
    size_t storage_capacity;
    ecat_foe_target_async_op_t op;
    uint8_t *read_destination;
    uint32_t *erased_end_destination;
    bool pending;
    bool failed;
} ecat_foe_target_async_t;

int ecat_foe_target_async_init(ecat_foe_target_async_t *runner,
                               const ecat_foe_target_async_ops_t *ops,
                               void *ctx,
                               uint8_t *storage,
                               size_t storage_capacity);

/* Start one operation. WRITE bytes are copied into runner-owned storage.
 * READ destination and ERASE_TO output pointers must remain valid until done.
 */
ecat_foe_target_async_result_t ecat_foe_target_async_start(
    ecat_foe_target_async_t *runner,
    ecat_foe_target_op_kind_t kind,
    uint32_t offset,
    uint32_t need_end,
    const uint8_t *data,
    uint32_t length,
    uint8_t *read_destination,
    uint32_t *erased_end_destination);

ecat_foe_target_async_result_t ecat_foe_target_async_start_finalize(
    ecat_foe_target_async_t *runner,
    uint16_t target_id,
    uint32_t fw_version,
    uint32_t min_allow_version,
    bool anti_rollback);

ecat_foe_target_async_result_t ecat_foe_target_async_poll(
    ecat_foe_target_async_t *runner);

/* Cancel a pending operation at most once; completed operations are untouched. */
void ecat_foe_target_async_abort(ecat_foe_target_async_t *runner);

/* Abort the current operation if pending and forward session abort to ops->abort_session.
 * Calls ops->abort at most once (while pending); forwarding to ops->abort_session occurs
 * per invocation. Upper-level session management (such as ecat_foe_core_async) enforces
 * session lifecycle and guarantees that abort_session is invoked at most once per session.
 */
void ecat_foe_target_async_abort_session(ecat_foe_target_async_t *runner);

/* Prepare a failed/aborted runner for a new session. Pending work must first
 * be cancelled; reset never silently abandons an in-flight driver operation.
 */
int ecat_foe_target_async_reset(ecat_foe_target_async_t *runner);

#ifdef __cplusplus
}
#endif
#endif
