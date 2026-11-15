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
 * @param ep 端点句柄
 * @param target_id 目标区块 ID
 * @param expected_size 预期写入总字节数 (0 表示按流式结束)
 * @param fw_version 固件版本号 (固件类目标有效)
 * @return ECAT_TRANSFER_COMPLETE 成功建立会话，其他为失败码
 */
ecat_transfer_result_t ecat_transport_endpoint_begin_import(
    ecat_transport_endpoint_t *ep,
    uint16_t target_id,
    uint32_t expected_size,
    uint32_t fw_version);

/**
 * @brief 分块推送导入数据流
 * @param ep 端点句柄
 * @param data 接收到的数据块指针
 * @param length 数据块长度
 * @return ECAT_TRANSFER_COMPLETE 成功，其他为错误码
 */
ecat_transfer_result_t ecat_transport_endpoint_write_chunk(
    ecat_transport_endpoint_t *ep,
    const uint8_t *data,
    size_t length);

/**
 * @brief 完成并提交导入会话
 * @param ep 端点句柄
 * @return ECAT_TRANSFER_COMPLETE 提交成功
 */
ecat_transfer_result_t ecat_transport_endpoint_finish_import(
    ecat_transport_endpoint_t *ep);

/**
 * @brief 发起数据导出回读会话 (从设备目标区块导出数据)
 * @param ep 端点句柄
 * @param target_id 目标区块 ID
 * @param out_total_size 传出实际可导出的总字节数
 * @return ECAT_TRANSFER_COMPLETE 成功建立导出通道
 */
ecat_transfer_result_t ecat_transport_endpoint_begin_export(
    ecat_transport_endpoint_t *ep,
    uint16_t target_id,
    uint32_t *out_total_size);

/**
 * @brief 分块拉取导出数据
 * @param ep 端点句柄
 * @param out_buf 输出缓冲区
 * @param buf_capacity 缓冲区容量
 * @param actual_read 传出本次实际读取的字节数
 * @return ECAT_TRANSFER_COMPLETE 数据已全部读取完毕;
 *         ECAT_TRANSFER_CONTINUE 成功读取当前块，后续仍有数据;
 *         其他为错误码
 */
ecat_transfer_result_t ecat_transport_endpoint_read_chunk(
    ecat_transport_endpoint_t *ep,
    uint8_t *out_buf,
    size_t buf_capacity,
    size_t *actual_read);

/**
 * @brief 完成导出回读会话
 * @param ep 端点句柄
 * @return ECAT_TRANSFER_COMPLETE
 */
ecat_transfer_result_t ecat_transport_endpoint_finish_export(
    ecat_transport_endpoint_t *ep);

/**
 * @brief 中止或取消当前端点的活动会话
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
 * @param ep 端点句柄
 * @return ECAT_TRANSFER_PENDING 驱动仍在处理;
 *         ECAT_TRANSFER_COMPLETE 异步阶段或终结操作完成;
 *         ECAT_TRANSFER_CONTINUE 异步读取阶段完成但后续仍有数据;
 *         ECAT_TRANSFER_INVALID 无活动会话或参数非法;
 *         其他为错误码 (同时触发 abort 事件通知并清理会话)
 */
ecat_transfer_result_t ecat_transport_endpoint_poll(ecat_transport_endpoint_t *ep);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_TRANSPORT_ENDPOINT_H */
