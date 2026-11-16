/** FoE bridge into the unified transfer manager. */
#ifndef ECAT_FOE_TRANSPORT_H
#define ECAT_FOE_TRANSPORT_H
#include "ecat_transfer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ecat_transfer_t *transfer;    /**< 权威统一双向传输管理器 (统一核心模式) */
    uint64_t token;
    bool pending;
    bool begin_pending;           /**< 标记当前正在进行 begin 异步等待 */
    bool finish_after_write;
    bool finishing;
    bool committed;
    uint8_t *staged_buf;          /**< 可选外部/调用方受控首包深拷贝暂存区 (纯同步保持 NULL 零多余 RAM) */
    size_t staged_cap;            /**< 可选外部/调用方受控首包暂存区容量 */
    size_t staged_length;         /**< 首包异步暂存数据实际有效长度 */
    bool staged_more_data;        /**< 首包后续数据标志 */
    bool has_staged_packet;       /**< 是否包含暂存的首包数据 */
    bool staged_shared;           /**< 标记暂存区是否与外部异步门控共享 (共享时 abort 不提前清零，供超时重试比对) */
} ecat_foe_transport_t;

/**
 * @brief 使用权威统一双向传输核心初始化 FoE 适配桥
 * @param transport 适配桥结构体指针
 * @param transfer 权威统一双向传输管理器
 * @return 0 成功, -1 参数非法
 */
int ecat_foe_transport_init_transfer(ecat_foe_transport_t *transport, ecat_transfer_t *transfer);

/**
 * @brief 为 FoE 适配桥配置首包异步受控暂存区 (独立独占缓冲区)
 * @note 纯同步部署保持未配置 (零多余 RAM 开销)；
 *       若发生异步首包暂存需求（begin 返回 PENDING）但未配置暂存区或容量不足，
 *       适配桥将严格安全失败并中止，绝不产生未定义内存行为。
 *       会话活跃期间（owns）严禁更换暂存区。
 * @param transport 适配桥结构体指针
 * @param buffer 外部受控暂存区指针 (NULL 表示清空)
 * @param capacity 暂存区容量 (字节)
 * @return 0 成功, -1 参数非法或处于活跃会话中
 */
int ecat_foe_transport_set_staged_buffer(ecat_foe_transport_t *transport, uint8_t *buffer, size_t capacity);

/**
 * @brief 为 FoE 适配桥配置首包异步共享暂存区 (如自动复用外部异步门控已有存储)
 * @note 共享暂存区在发生 abort 时不会被提前清零，保留原始包数据供外部异步门控进行超时重传比对，
 *       数据生命周期在外部异步门控完成握手闭环 (clear_packet) 时安全清理。
 * @param transport 适配桥结构体指针
 * @param buffer 外部共享暂存区指针
 * @param capacity 暂存区容量 (字节)
 * @return 0 成功, -1 参数非法或处于活跃会话中
 */
int ecat_foe_transport_set_shared_staged_buffer(ecat_foe_transport_t *transport, uint8_t *buffer, size_t capacity);

/* Returns FoE wire codes; a new WRQ can replace only this bridge's session. */
int ecat_foe_transport_begin(ecat_foe_transport_t *transport, const char *name, size_t length);
ecat_upgrade_async_result_t ecat_foe_transport_start(void *ctx, const uint8_t *data,
    size_t length, bool more_data);
ecat_upgrade_async_result_t ecat_foe_transport_poll(void *ctx);
void ecat_foe_transport_abort(void *ctx);
bool ecat_foe_transport_owns(const ecat_foe_transport_t *transport);
/* Suitable for the existing SSC retry gate; final packet waits for commit. */
const ecat_upgrade_async_ops_t *ecat_foe_transport_async_ops(void);
#ifdef __cplusplus
}
#endif
#endif
