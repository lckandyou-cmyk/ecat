/*
 * FoE 固件升级专用适配器实现
 *
 * 架构说明与迁移边界:
 * 本实现为既有 EtherCAT FoE 工业总线协议层提供双模式桥接：
 * 1. 统一核心模式 (ecat_foe_transport_init_transfer): FoE 协议栈直接接入权威统一传输核心
 *    (ecat_transfer_t)，完全受管于统一状态机、统一 ACL 矩阵与全生命周期；
 * 2. 遗留兼容模式 (ecat_foe_transport_init): 保留对原有 ecat_upgrade_t 的接入路径，两者在底层
 *    共享全局单一租约仲裁器 (ecat_upgrade_session_t) 与 core，在租约层实现强绑定与排他互斥；
 * 3. 明确当前边界：原有旧接口保留以确保对现有 Beckhoff SSC / SOES 部署 100% 行为兼容。
 */
#include "ecat_foe_transport.h"
#include <string.h>

int ecat_foe_transport_init(ecat_foe_transport_t *t, ecat_upgrade_t *upgrade)
{
    if (t == NULL || upgrade == NULL || upgrade->core == NULL) return -1;
    memset(t, 0, sizeof(*t));
    t->upgrade = upgrade;
    return 0;
}

int ecat_foe_transport_init_transfer(ecat_foe_transport_t *t, ecat_transfer_t *transfer)
{
    if (t == NULL || transfer == NULL || transfer->core == NULL) return -1;
    memset(t, 0, sizeof(*t));
    t->transfer = transfer;
    return 0;
}

static ecat_upgrade_session_t *get_transport_session(const ecat_foe_transport_t *t)
{
    if (t->transfer != NULL) return t->transfer->session_ptr;
    if (t->upgrade != NULL) {
        return t->upgrade->session_ptr ? t->upgrade->session_ptr : &t->upgrade->session;
    }
    return NULL;
}

static const void *get_manager_ptr(const ecat_foe_transport_t *t)
{
    if (t->transfer != NULL) return t->transfer;
    return t->upgrade;
}

static ecat_foe_core_t *get_transport_core(const ecat_foe_transport_t *t)
{
    if (t->transfer != NULL) return t->transfer->core;
    if (t->upgrade != NULL) return t->upgrade->core;
    return NULL;
}

bool ecat_foe_transport_owns(const ecat_foe_transport_t *t)
{
    if (t == NULL) return false;
    ecat_upgrade_session_t *sess = get_transport_session(t);
    const void *mgr = get_manager_ptr(t);
    if (sess == NULL || mgr == NULL) return false;
    return ecat_upgrade_session_owns_mgr(sess, mgr, t, t->token);
}

void ecat_foe_transport_abort(void *ctx)
{
    ecat_foe_transport_t *t = (ecat_foe_transport_t *)ctx;
    if (t == NULL) return;
    if (ecat_foe_transport_owns(t)) {
        if (t->transfer != NULL) {
            (void)ecat_transfer_cancel(t->transfer, t, t->token);
        } else if (t->upgrade != NULL) {
            (void)ecat_upgrade_cancel(t->upgrade, t, t->token);
        }
    }
    t->pending = false;
    t->finish_after_write = false;
    t->finishing = false;
}

int ecat_foe_transport_begin(ecat_foe_transport_t *t, const char *name, size_t length)
{
    if (t == NULL || (t->transfer == NULL && t->upgrade == NULL) || name == NULL) {
        return ECAT_FOE_ERRCODE_ILLEGAL;
    }
    ecat_upgrade_session_t *sess = get_transport_session(t);
    if (sess != NULL && sess->owner != NULL && !ecat_foe_transport_owns(t)) {
        return ECAT_FOE_ERRCODE_NOTDEFINED;
    }
    ecat_foe_transport_abort(t);
    t->committed = false;
    ecat_foe_core_t *core = get_transport_core(t);
    if (core == NULL) return ECAT_FOE_ERRCODE_NOTDEFINED;

    uint16_t target_id = 0;
    uint32_t fw_version = 0;
    int result = ecat_foe_core_validate_wrq(core, name, length, &target_id, &fw_version);
    if (result != 0) return result;

    if (t->transfer != NULL) {
        ecat_transfer_request_t req;
        memset(&req, 0, sizeof(req));
        req.transport_id = 1U; /* FoE 通道 ID */
        req.target_id = target_id;
        req.direction = ECAT_TRANSFER_DIR_IMPORT;
        req.expected_size = 0U;
        req.fw_version = fw_version;

        ecat_transfer_result_t tres = ecat_transfer_begin(t->transfer, t, &req, &t->token);
        return (tres == ECAT_TRANSFER_COMPLETE || tres == ECAT_TRANSFER_PENDING) ? 0 : ECAT_FOE_ERRCODE_NOTDEFINED;
    } else {
        ecat_upgrade_request_t request;
        request.target_id = target_id;
        request.fw_version = fw_version;
        return ecat_upgrade_begin(t->upgrade, t, &request, &t->token) == ECAT_UPGRADE_COMPLETE
            ? 0 : ECAT_FOE_ERRCODE_NOTDEFINED;
    }
}

static ecat_upgrade_async_result_t advance(ecat_foe_transport_t *t, ecat_upgrade_result_t result)
{
    if (result == ECAT_UPGRADE_PENDING) {
        t->pending = true;
        return ECAT_UPGRADE_ASYNC_PENDING;
    }
    t->pending = false;
    if (result != ECAT_UPGRADE_COMPLETE) {
        ecat_foe_transport_abort(t);
        return ECAT_UPGRADE_ASYNC_FAILED;
    }
    if (t->finish_after_write && !t->finishing) {
        t->finishing = true;
        if (t->transfer != NULL) {
            ecat_transfer_result_t fres = ecat_transfer_finish(t->transfer, t, t->token);
            if (fres == ECAT_TRANSFER_PENDING) {
                t->pending = true;
                return ECAT_UPGRADE_ASYNC_PENDING;
            } else if (fres == ECAT_TRANSFER_COMPLETE) {
                t->committed = true;
                return ECAT_UPGRADE_ASYNC_COMPLETE;
            } else {
                ecat_foe_transport_abort(t);
                return ECAT_UPGRADE_ASYNC_FAILED;
            }
        } else {
            return advance(t, ecat_upgrade_finish(t->upgrade, t, t->token));
        }
    }
    if (t->finishing) t->committed = true;
    return ECAT_UPGRADE_ASYNC_COMPLETE;
}

ecat_upgrade_async_result_t ecat_foe_transport_start(void *ctx, const uint8_t *data,
    size_t length, bool more_data)
{
    ecat_foe_transport_t *t = (ecat_foe_transport_t *)ctx;
    if (!ecat_foe_transport_owns(t) || t->pending) return ECAT_UPGRADE_ASYNC_FAILED;
    t->finish_after_write = !more_data;
    t->finishing = false;

    if (t->transfer != NULL) {
        ecat_transfer_result_t res = ecat_transfer_write(t->transfer, t, t->token, data, length);
        if (res == ECAT_TRANSFER_PENDING) {
            t->pending = true;
            return ECAT_UPGRADE_ASYNC_PENDING;
        } else if (res == ECAT_TRANSFER_COMPLETE) {
            t->pending = false;
            if (t->finish_after_write) {
                t->finishing = true;
                ecat_transfer_result_t fres = ecat_transfer_finish(t->transfer, t, t->token);
                if (fres == ECAT_TRANSFER_PENDING) {
                    t->pending = true;
                    return ECAT_UPGRADE_ASYNC_PENDING;
                } else if (fres == ECAT_TRANSFER_COMPLETE) {
                    t->committed = true;
                    return ECAT_UPGRADE_ASYNC_COMPLETE;
                } else {
                    ecat_foe_transport_abort(t);
                    return ECAT_UPGRADE_ASYNC_FAILED;
                }
            }
            return ECAT_UPGRADE_ASYNC_COMPLETE;
        } else {
            ecat_foe_transport_abort(t);
            return ECAT_UPGRADE_ASYNC_FAILED;
        }
    } else {
        return advance(t, ecat_upgrade_write(t->upgrade, t, t->token, data, length));
    }
}

ecat_upgrade_async_result_t ecat_foe_transport_poll(void *ctx)
{
    ecat_foe_transport_t *t = (ecat_foe_transport_t *)ctx;
    if (t == NULL || !t->pending || !ecat_foe_transport_owns(t)) return ECAT_UPGRADE_ASYNC_FAILED;

    if (t->transfer != NULL) {
        ecat_transfer_result_t res = ecat_transfer_poll(t->transfer, t, t->token);
        if (res == ECAT_TRANSFER_PENDING) {
            return ECAT_UPGRADE_ASYNC_PENDING;
        } else if (res == ECAT_TRANSFER_COMPLETE) {
            t->pending = false;
            if (t->finish_after_write && !t->finishing) {
                t->finishing = true;
                ecat_transfer_result_t fres = ecat_transfer_finish(t->transfer, t, t->token);
                if (fres == ECAT_TRANSFER_PENDING) {
                    t->pending = true;
                    return ECAT_UPGRADE_ASYNC_PENDING;
                } else if (fres == ECAT_TRANSFER_COMPLETE) {
                    t->committed = true;
                    return ECAT_UPGRADE_ASYNC_COMPLETE;
                } else {
                    ecat_foe_transport_abort(t);
                    return ECAT_UPGRADE_ASYNC_FAILED;
                }
            }
            if (t->finishing) t->committed = true;
            return ECAT_UPGRADE_ASYNC_COMPLETE;
        } else {
            ecat_foe_transport_abort(t);
            return ECAT_UPGRADE_ASYNC_FAILED;
        }
    } else {
        return advance(t, ecat_upgrade_poll(t->upgrade, t, t->token));
    }
}

const ecat_upgrade_async_ops_t *ecat_foe_transport_async_ops(void)
{
    static const ecat_upgrade_async_ops_t ops = {
        ecat_foe_transport_start, ecat_foe_transport_poll, ecat_foe_transport_abort
    };
    return &ops;
}
