/**
 * @file ecat_transport_endpoint.c
 * @brief 通用传输端点抽象与自定义通道适配层实现
 */

#include "ecat_transport_endpoint.h"
#include <string.h>

int ecat_transport_endpoint_init(
    ecat_transport_endpoint_t *ep,
    uint16_t transport_id,
    const char *name,
    ecat_transfer_t *mgr,
    void *user_ctx)
{
    if (ep == NULL || mgr == NULL || name == NULL || name[0] == '\0') {
        return -1;
    }

    memset(ep, 0, sizeof(*ep));
    ep->transport_id = transport_id;
    strncpy(ep->name, name, ECAT_TRANSPORT_NAME_MAX_LEN - 1U);
    ep->name[ECAT_TRANSPORT_NAME_MAX_LEN - 1U] = '\0';
    ep->transfer_mgr = mgr;
    ep->user_ctx = user_ctx;
    ep->state = ECAT_TRANSPORT_STATE_IDLE;

    return 0;
}

void ecat_transport_endpoint_set_callbacks(
    ecat_transport_endpoint_t *ep,
    const ecat_transport_callbacks_t *cbs)
{
    if (ep == NULL) {
        return;
    }

    if (cbs != NULL) {
        ep->cbs = *cbs;
    } else {
        memset(&ep->cbs, 0, sizeof(ep->cbs));
    }
}

ecat_transfer_result_t ecat_transport_endpoint_begin_import(
    ecat_transport_endpoint_t *ep,
    uint16_t target_id,
    uint32_t expected_size,
    uint32_t fw_version)
{
    if (ep == NULL || ep->transfer_mgr == NULL) {
        return ECAT_TRANSFER_INVALID;
    }

    /* 若之前有残留未完成会话，先安全取消 */
    if (ep->current_token != 0U) {
        ecat_transport_endpoint_abort(ep);
    }

    ecat_transfer_request_t req;
    req.transport_id = ep->transport_id;
    req.target_id = target_id;
    req.direction = ECAT_TRANSFER_DIR_IMPORT;
    req.expected_size = expected_size;
    req.fw_version = fw_version;

    uint64_t token = 0U;
    ecat_transfer_result_t res = ecat_transfer_begin(
        ep->transfer_mgr, ep, &req, &token);

    if (res == ECAT_TRANSFER_COMPLETE) {
        ep->current_token = token;
        ep->state = ECAT_TRANSPORT_STATE_IMPORTING;
        ep->total_size = expected_size;
        ep->begin_pending = false;
        ep->finishing = false;
        if (ep->cbs.on_session_start != NULL) {
            ep->cbs.on_session_start(ep->user_ctx, ECAT_TRANSFER_DIR_IMPORT, target_id);
        }
    } else if (res == ECAT_TRANSFER_PENDING) {
        /* 异步启动挂起 (如 INIT/INVALIDATE 异步执行): 保存 token 并保持挂起导入状态，等待 poll 推进 */
        ep->current_token = token;
        ep->state = ECAT_TRANSPORT_STATE_IMPORTING;
        ep->total_size = expected_size;
        ep->begin_pending = true;
        ep->pending_target_id = target_id;
        ep->finishing = false;
    } else if (res == ECAT_TRANSFER_BUSY) {
        /* 繁忙/冲突: 保留原端点状态与 token，不触发 abort 回调 */
        return res;
    } else {
        ep->state = ECAT_TRANSPORT_STATE_ERROR;
        ep->current_token = 0U;
        ep->begin_pending = false;
        ep->finishing = false;
        if (ep->cbs.on_session_abort != NULL) {
            ep->cbs.on_session_abort(ep->user_ctx, ecat_transfer_get_last_error(ep->transfer_mgr));
        }
    }

    return res;
}

ecat_transfer_result_t ecat_transport_endpoint_write_chunk(
    ecat_transport_endpoint_t *ep,
    const uint8_t *data,
    size_t length)
{
    if (ep == NULL || ep->state != ECAT_TRANSPORT_STATE_IMPORTING) {
        return ECAT_TRANSFER_INVALID;
    }

    if (ep->begin_pending) {
        /* 会话启动尚未就绪: 禁止分块写入，直接返回 BUSY 保留会话与状态 */
        return ECAT_TRANSFER_BUSY;
    }

    ecat_transfer_result_t res = ecat_transfer_write(
        ep->transfer_mgr, ep, ep->current_token, data, length);

    if (res == ECAT_TRANSFER_COMPLETE) {
        if (ep->cbs.on_progress != NULL) {
            uint32_t cur = ecat_transfer_get_bytes_transferred(ep->transfer_mgr);
            ep->cbs.on_progress(ep->user_ctx, cur, ep->total_size);
        }
        return res;
    }

    if (res == ECAT_TRANSFER_PENDING) {
        /* 异步操作挂起中: 保持 IMPORTING 状态，等待 poll 推进 */
        return res;
    }

    if (res == ECAT_TRANSFER_BUSY) {
        /* 并发或挂起时重复操作: 保留原操作、会话状态与令牌，直接返回 BUSY */
        return res;
    }

    if (res == ECAT_TRANSFER_INVALID) {
        /* 可重试参数错误: 保留会话与 IMPORTING 状态，允许调用方更正参数重试或调用 abort 清理 */
        return res;
    }

    /* 致命/终止错误: 清 token 前必须显式释放/取消会话 */
    if (ep->current_token != 0U && ep->transfer_mgr != NULL) {
        (void)ecat_transfer_cancel(ep->transfer_mgr, ep, ep->current_token);
    }
    ep->state = ECAT_TRANSPORT_STATE_ERROR;
    ep->current_token = 0U;
    if (ep->cbs.on_session_abort != NULL) {
        ep->cbs.on_session_abort(ep->user_ctx, ecat_transfer_get_last_error(ep->transfer_mgr));
    }

    return res;
}

ecat_transfer_result_t ecat_transport_endpoint_finish_import(
    ecat_transport_endpoint_t *ep)
{
    if (ep == NULL || ep->state != ECAT_TRANSPORT_STATE_IMPORTING) {
        return ECAT_TRANSFER_INVALID;
    }

    if (ep->begin_pending) {
        /* 会话启动尚未就绪: 禁止终结，直接返回 BUSY */
        return ECAT_TRANSFER_BUSY;
    }

    ecat_transfer_result_t res = ecat_transfer_finish(
        ep->transfer_mgr, ep, ep->current_token);

    if (res == ECAT_TRANSFER_COMPLETE) {
        uint32_t total = ecat_transfer_get_bytes_transferred(ep->transfer_mgr);
        uint32_t crc = ecat_transfer_get_crc32(ep->transfer_mgr);
        ep->state = ECAT_TRANSPORT_STATE_IDLE;
        ep->current_token = 0U;
        ep->finishing = false;
        if (ep->cbs.on_session_complete != NULL) {
            ep->cbs.on_session_complete(ep->user_ctx, total, crc);
        }
        return res;
    }

    if (res == ECAT_TRANSFER_PENDING) {
        /* 异步提交进行中: 标记 finishing，保持 IMPORTING 等待 poll 推进终结 */
        ep->finishing = true;
        return res;
    }

    if (res == ECAT_TRANSFER_BUSY) {
        /* 保持原会话与 finishing 状态 */
        return res;
    }

    ep->finishing = false;
    if (ep->current_token != 0U && ep->transfer_mgr != NULL) {
        (void)ecat_transfer_cancel(ep->transfer_mgr, ep, ep->current_token);
    }
    ep->state = ECAT_TRANSPORT_STATE_ERROR;
    ep->current_token = 0U;
    if (ep->cbs.on_session_abort != NULL) {
        ep->cbs.on_session_abort(ep->user_ctx, ecat_transfer_get_last_error(ep->transfer_mgr));
    }

    return res;
}

ecat_transfer_result_t ecat_transport_endpoint_begin_export(
    ecat_transport_endpoint_t *ep,
    uint16_t target_id,
    uint32_t *out_total_size)
{
    if (ep == NULL || ep->transfer_mgr == NULL) {
        return ECAT_TRANSFER_INVALID;
    }

    if (ep->current_token != 0U) {
        ecat_transport_endpoint_abort(ep);
    }

    ecat_transfer_request_t req;
    req.transport_id = ep->transport_id;
    req.target_id = target_id;
    req.direction = ECAT_TRANSFER_DIR_EXPORT;
    req.expected_size = 0U;
    req.fw_version = 0U;

    uint64_t token = 0U;
    ecat_transfer_result_t res = ecat_transfer_begin(
        ep->transfer_mgr, ep, &req, &token);

    if (res == ECAT_TRANSFER_COMPLETE) {
        ep->current_token = token;
        ep->state = ECAT_TRANSPORT_STATE_EXPORTING;
        ep->total_size = ecat_transfer_get_total_export_size(ep->transfer_mgr);
        ep->begin_pending = false;
        ep->finishing = false;
        if (out_total_size != NULL) {
            *out_total_size = ep->total_size;
        }
        if (ep->cbs.on_session_start != NULL) {
            ep->cbs.on_session_start(ep->user_ctx, ECAT_TRANSFER_DIR_EXPORT, target_id);
        }
    } else if (res == ECAT_TRANSFER_PENDING) {
        /* 异步导出只读打开挂起 (如 READ_OPEN 异步执行): 保存 token 并保持挂起导出状态，等待 poll 推进 */
        ep->current_token = token;
        ep->state = ECAT_TRANSPORT_STATE_EXPORTING;
        ep->total_size = ecat_transfer_get_total_export_size(ep->transfer_mgr);
        ep->begin_pending = true;
        ep->pending_target_id = target_id;
        ep->finishing = false;
        if (out_total_size != NULL) {
            *out_total_size = ep->total_size;
        }
    } else if (res == ECAT_TRANSFER_BUSY) {
        return res;
    } else {
        ep->state = ECAT_TRANSPORT_STATE_ERROR;
        ep->current_token = 0U;
        ep->begin_pending = false;
        ep->finishing = false;
        if (ep->cbs.on_session_abort != NULL) {
            ep->cbs.on_session_abort(ep->user_ctx, ecat_transfer_get_last_error(ep->transfer_mgr));
        }
    }

    return res;
}

ecat_transfer_result_t ecat_transport_endpoint_read_chunk(
    ecat_transport_endpoint_t *ep,
    uint8_t *out_buf,
    size_t buf_capacity,
    size_t *actual_read)
{
    if (ep == NULL || ep->state != ECAT_TRANSPORT_STATE_EXPORTING) {
        return ECAT_TRANSFER_INVALID;
    }

    if (ep->begin_pending) {
        /* 只读会话启动尚未就绪: 禁止分块读取，直接返回 BUSY */
        return ECAT_TRANSFER_BUSY;
    }

    ecat_transfer_result_t res = ecat_transfer_read(
        ep->transfer_mgr, ep, ep->current_token, out_buf, buf_capacity, actual_read);

    if (res == ECAT_TRANSFER_COMPLETE || res == ECAT_TRANSFER_CONTINUE) {
        if (ep->cbs.on_progress != NULL) {
            uint32_t cur = ecat_transfer_get_bytes_transferred(ep->transfer_mgr);
            ep->cbs.on_progress(ep->user_ctx, cur, ep->total_size);
        }
        return res;
    }

    if (res == ECAT_TRANSFER_PENDING) {
        /* 异步挂起: 保持 EXPORTING 状态，等待 poll 推进 */
        return res;
    }

    if (res == ECAT_TRANSFER_BUSY) {
        /* 保留原导出读取会话与令牌 */
        return res;
    }

    if (res == ECAT_TRANSFER_INVALID) {
        /* 可重试参数错误: 保留会话与 EXPORTING 状态，允许调用方更正参数重试或调用 abort 清理 */
        return res;
    }

    /* 致命/终止错误: 清 token 前必须显式释放/取消会话 */
    if (ep->current_token != 0U && ep->transfer_mgr != NULL) {
        (void)ecat_transfer_cancel(ep->transfer_mgr, ep, ep->current_token);
    }
    ep->state = ECAT_TRANSPORT_STATE_ERROR;
    ep->current_token = 0U;
    if (ep->cbs.on_session_abort != NULL) {
        ep->cbs.on_session_abort(ep->user_ctx, ecat_transfer_get_last_error(ep->transfer_mgr));
    }

    return res;
}

ecat_transfer_result_t ecat_transport_endpoint_finish_export(
    ecat_transport_endpoint_t *ep)
{
    if (ep == NULL || ep->state != ECAT_TRANSPORT_STATE_EXPORTING) {
        return ECAT_TRANSFER_INVALID;
    }

    if (ep->begin_pending) {
        /* 会话启动尚未就绪: 禁止终结，直接返回 BUSY */
        return ECAT_TRANSFER_BUSY;
    }

    ecat_transfer_result_t res = ecat_transfer_finish(
        ep->transfer_mgr, ep, ep->current_token);

    if (res == ECAT_TRANSFER_COMPLETE) {
        uint32_t total = ecat_transfer_get_bytes_transferred(ep->transfer_mgr);
        uint32_t crc = ecat_transfer_get_crc32(ep->transfer_mgr);
        ep->state = ECAT_TRANSPORT_STATE_IDLE;
        ep->current_token = 0U;
        ep->finishing = false;
        if (ep->cbs.on_session_complete != NULL) {
            ep->cbs.on_session_complete(ep->user_ctx, total, crc);
        }
        return res;
    }

    if (res == ECAT_TRANSFER_PENDING) {
        /* 异步提交进行中: 标记 finishing，保持 EXPORTING 等待 poll 推进终结 */
        ep->finishing = true;
        return res;
    }

    if (res == ECAT_TRANSFER_BUSY) {
        /* 保留原导出终结状态与令牌 */
        return res;
    }

    ep->finishing = false;
    if (ep->current_token != 0U && ep->transfer_mgr != NULL) {
        (void)ecat_transfer_cancel(ep->transfer_mgr, ep, ep->current_token);
    }
    ep->state = ECAT_TRANSPORT_STATE_ERROR;
    ep->current_token = 0U;
    if (ep->cbs.on_session_abort != NULL) {
        ep->cbs.on_session_abort(ep->user_ctx, ecat_transfer_get_last_error(ep->transfer_mgr));
    }

    return res;
}

void ecat_transport_endpoint_abort(ecat_transport_endpoint_t *ep)
{
    if (ep == NULL) {
        return;
    }

    bool was_active = (ep->state == ECAT_TRANSPORT_STATE_IMPORTING ||
                       ep->state == ECAT_TRANSPORT_STATE_EXPORTING ||
                       ep->current_token != 0U);

    ep->finishing = false;
    ep->begin_pending = false;
    if (ep->current_token != 0U && ep->transfer_mgr != NULL) {
        (void)ecat_transfer_cancel(ep->transfer_mgr, ep, ep->current_token);
        ep->current_token = 0U;
    }

    ep->state = ECAT_TRANSPORT_STATE_IDLE;
    if (was_active && ep->cbs.on_session_abort != NULL) {
        ep->cbs.on_session_abort(ep->user_ctx, ECAT_TRANSFER_ERR_NONE);
    }
}

bool ecat_transport_endpoint_is_active(const ecat_transport_endpoint_t *ep)
{
    if (ep == NULL) {
        return false;
    }
    return (ep->current_token != 0U &&
            (ep->state == ECAT_TRANSPORT_STATE_IMPORTING ||
             ep->state == ECAT_TRANSPORT_STATE_EXPORTING));
}

bool ecat_transport_endpoint_is_pending(const ecat_transport_endpoint_t *ep)
{
    if (ep == NULL || ep->transfer_mgr == NULL) {
        return false;
    }
    if (!ecat_transport_endpoint_is_active(ep)) {
        return false;
    }
    return ecat_transfer_is_pending(ep->transfer_mgr);
}

ecat_transfer_result_t ecat_transport_endpoint_poll(ecat_transport_endpoint_t *ep)
{
    if (ep == NULL || ep->transfer_mgr == NULL) {
        return ECAT_TRANSFER_INVALID;
    }
    if (!ecat_transport_endpoint_is_active(ep)) {
        return ECAT_TRANSFER_INVALID;
    }

    ecat_transfer_result_t res = ecat_transfer_poll(
        ep->transfer_mgr, ep, ep->current_token);

    if (res == ECAT_TRANSFER_PENDING) {
        return ECAT_TRANSFER_PENDING;
    }

    if (res == ECAT_TRANSFER_COMPLETE) {
        if (ep->begin_pending) {
            /* 启动阶段推进完成: 清除 begin_pending 并单次触发 on_session_start */
            ep->begin_pending = false;
            if (ep->cbs.on_session_start != NULL) {
                ecat_transfer_dir_t dir = (ep->state == ECAT_TRANSPORT_STATE_IMPORTING) ?
                                          ECAT_TRANSFER_DIR_IMPORT : ECAT_TRANSFER_DIR_EXPORT;
                ep->cbs.on_session_start(ep->user_ctx, dir, ep->pending_target_id);
            }
            return ECAT_TRANSFER_COMPLETE;
        }

        if (ep->finishing) {
            /* 终结阶段完成: 派发单次完成通知，恢复 IDLE */
            uint32_t total = ecat_transfer_get_bytes_transferred(ep->transfer_mgr);
            uint32_t crc = ecat_transfer_get_crc32(ep->transfer_mgr);
            ep->state = ECAT_TRANSPORT_STATE_IDLE;
            ep->current_token = 0U;
            ep->finishing = false;
            if (ep->cbs.on_session_complete != NULL) {
                ep->cbs.on_session_complete(ep->user_ctx, total, crc);
            }
            return ECAT_TRANSFER_COMPLETE;
        }

        /* 普通分块 I/O 推进完成 */
        if (ep->cbs.on_progress != NULL) {
            uint32_t cur = ecat_transfer_get_bytes_transferred(ep->transfer_mgr);
            ep->cbs.on_progress(ep->user_ctx, cur, ep->total_size);
        }
        return ECAT_TRANSFER_COMPLETE;
    }

    if (res == ECAT_TRANSFER_CONTINUE) {
        if (ep->cbs.on_progress != NULL) {
            uint32_t cur = ecat_transfer_get_bytes_transferred(ep->transfer_mgr);
            ep->cbs.on_progress(ep->user_ctx, cur, ep->total_size);
        }
        return ECAT_TRANSFER_CONTINUE;
    }

    if (res == ECAT_TRANSFER_BUSY) {
        return ECAT_TRANSFER_BUSY;
    }

    /* 异步发生错误: 清理挂起标记，取消会话并派发中止回调 */
    ep->begin_pending = false;
    ep->finishing = false;
    if (ep->current_token != 0U && ep->transfer_mgr != NULL) {
        (void)ecat_transfer_cancel(ep->transfer_mgr, ep, ep->current_token);
    }
    ep->state = ECAT_TRANSPORT_STATE_ERROR;
    ep->current_token = 0U;
    if (ep->cbs.on_session_abort != NULL) {
        ep->cbs.on_session_abort(ep->user_ctx, ecat_transfer_get_last_error(ep->transfer_mgr));
    }
    return res;
}
