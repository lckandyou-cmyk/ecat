/**
 * @file ecat_transfer.c
 * @brief 嵌入式多通道双向传输管理器实现
 */

#include "ecat_transfer.h"
#include "ecat_foe_efw.h"
#include "ecat_foe_core_async.h"
#include <string.h>

static bool target_registry_acl_adapter(
    const void *registry,
    uint16_t transport_id,
    uint16_t target_id,
    uint8_t required_perm)
{
    return ecat_target_registry_check_permission(
        (const ecat_target_registry_t *)registry,
        transport_id,
        target_id,
        required_perm);
}

/**
 * @brief 动态计算固件容器物理上限 (header + payload)
 *
 * 依据 EFW v0.2 规范 (docs/spec/efw-format-v0.2.md:53)，固件容器头部恒定为 148 字节 (ECAT_FOE_EFW_HEADER_MIN_LEN)；
 * 空包规避填充若存在亦计入 payload_length，容器传输流中绝不存在独立的尾部未覆盖填充区，
 * 传输流总字节数恒定有界于 header_length + payload_length。
 * 核心驱动层对物理介质不足 write_unit 的尾部对齐填充仅发生在设备内部缓冲写入阶段，不属于传输流容器尺寸。
 * 因此容器流物理总上限精确为: eff_payload + 148U。
 * 统一推导规则，使用 uint64 宽算术彻底防范 32 位整型加法溢出回绕。
 */
static uint32_t calc_firmware_container_limit(
    const ecat_target_desc_t *target,
    const ecat_foe_core_t *core)
{
    uint32_t eff_payload = target->max_size;
    uint32_t max_hdr = ECAT_FOE_EFW_HEADER_MIN_LEN;

    if (core != NULL) {
        if (target->target_id >= 1U && target->target_id <= ECAT_FOE_TARGET_COUNT) {
            uint32_t target_idx = (uint32_t)target->target_id - 1U;
            if (core->cfg.targets[target_idx].max_payload_size > 0U &&
                core->cfg.targets[target_idx].max_payload_size < eff_payload) {
                eff_payload = core->cfg.targets[target_idx].max_payload_size;
            }
        }
    }

    uint64_t wide_cap = (uint64_t)eff_payload + (uint64_t)max_hdr;
    return (wide_cap > (uint64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)wide_cap;
}

int ecat_transfer_init(ecat_transfer_t *t, ecat_target_registry_t *reg, ecat_foe_core_t *core)
{
    if (t == NULL || reg == NULL) {
        return -1;
    }

    if (core != NULL) {
        if (core->registry != NULL && core->registry != reg) {
            /* 严禁冲突绑定: 核心状态机已绑定不同的权威注册表，杜绝 last-writer-wins 劫持 */
            return -1;
        }
    }

    memset(t, 0, sizeof(*t));
    t->registry = reg;
    t->core = core;
    t->last_error = ECAT_TRANSFER_ERR_NONE;

    if (core != NULL) {
        core->registry = reg;
        core->acl_check = target_registry_acl_adapter;
        if (core->shared_session == NULL) {
            core->shared_session = &t->session;
        }
        t->session_ptr = (ecat_upgrade_session_t *)core->shared_session;
    } else {
        t->session_ptr = &t->session;
    }

    return 0;
}

int ecat_transfer_set_staging_buffer(
    ecat_transfer_t *t,
    uint8_t *buf,
    size_t capacity)
{
    if (t == NULL || buf == NULL || capacity == 0U) {
        return -1;
    }
    if (t->pending) {
        return -1;
    }
    t->staging_buf = buf;
    t->staging_capacity = capacity;
    return 0;
}

int ecat_transfer_init_async_firmware(
    ecat_transfer_t *t,
    const ecat_upgrade_async_ops_t *ops,
    void *ctx)
{
    if (t == NULL || t->core == NULL || ops == NULL || ops->start == NULL ||
        ops->poll == NULL || ops->abort == NULL || ctx == NULL) {
        return -1;
    }

    const ecat_upgrade_async_ops_t *auth_ops = ecat_foe_core_async_ops();
    if (auth_ops == NULL ||
        ops->start != auth_ops->start ||
        ops->poll != auth_ops->poll ||
        ops->abort != auth_ops->abort ||
        t->core->async_abort_hook != auth_ops->abort ||
        t->core->async_abort_ctx != ctx) {
        return -1;
    }

    t->fw_async_ops = ops;
    t->fw_async_ctx = ctx;
    return 0;
}

bool ecat_transfer_is_pending(const ecat_transfer_t *t)
{
    return t != NULL && t->pending;
}

static inline ecat_upgrade_session_t *get_transfer_session(ecat_transfer_t *t)
{
    return (t != NULL && t->session_ptr != NULL) ? t->session_ptr : (t != NULL ? &t->session : NULL);
}

static inline const ecat_upgrade_session_t *get_transfer_session_const(const ecat_transfer_t *t)
{
    return (t != NULL && t->session_ptr != NULL) ? t->session_ptr : (t != NULL ? &t->session : NULL);
}

static void release_session(ecat_transfer_t *t, const void *owner, uint64_t token)
{
    if (t != NULL) {
        if (t->core_cap_overridden && t->core != NULL) {
            t->core->cfg.targets[t->saved_core_target_idx].max_payload_size = t->saved_core_max_payload;
            t->core_cap_overridden = false;
        }
        ecat_upgrade_session_t *s = get_transfer_session(t);
        if (s != NULL) {
            (void)ecat_upgrade_session_release_mgr(s, t, owner, token);
        }
    }
}

static bool is_owned(const ecat_transfer_t *t, const void *owner, uint64_t token)
{
    return (t != NULL && ecat_upgrade_session_owns_mgr(get_transfer_session_const(t), t, owner, token));
}

ecat_transfer_result_t ecat_transfer_begin(
    ecat_transfer_t *t,
    const void *owner,
    const ecat_transfer_request_t *request,
    uint64_t *token)
{
    if (t == NULL || owner == NULL || request == NULL || token == NULL) {
        return ECAT_TRANSFER_INVALID;
    }

    /* 在任何会话获取、驱动调用或副作用前严格校验方向枚举有效性 */
    if (request->direction != ECAT_TRANSFER_DIR_IMPORT &&
        request->direction != ECAT_TRANSFER_DIR_EXPORT) {
        return ECAT_TRANSFER_INVALID;
    }

    /* 检查 core 是否已被其它入口占用进行升级 */
    if (t->core != NULL) {
        if (t->core->state == ECAT_FOE_STATE_WRQ_ACKED ||
            t->core->state == ECAT_FOE_STATE_DATA ||
            t->core->session_active) {
            return ECAT_TRANSFER_BUSY;
        }
        if (t->core->state == ECAT_FOE_STATE_SUCCESS || t->core->state == ECAT_FOE_STATE_ERROR) {
            ecat_foe_core_on_abort(t->core);
        }
    }

    uint64_t acquired_token = 0;
    ecat_upgrade_session_result_t session_res =
        ecat_upgrade_session_acquire_mgr(get_transfer_session(t), t, owner, &acquired_token);

    if (session_res == ECAT_UPGRADE_SESSION_BUSY) {
        return ECAT_TRANSFER_BUSY;
    }
    if (session_res != ECAT_UPGRADE_SESSION_OK) {
        return ECAT_TRANSFER_FAILED;
    }

    /* 查找目标区块描述符 */
    const ecat_target_desc_t *target =
        ecat_target_registry_find_by_id(t->registry, request->target_id);
    if (target == NULL || !target->enabled) {
        t->last_error = ECAT_TRANSFER_ERR_TARGET_NOT_FOUND;
        release_session(t, owner, acquired_token);
        return ECAT_TRANSFER_FAILED;
    }

    /* 权限检查 (ACL 与目标权限) */
    uint8_t required_perm = (request->direction == ECAT_TRANSFER_DIR_IMPORT) ?
        ECAT_PERM_WRITE : ECAT_PERM_READ;

    if (!ecat_target_registry_check_permission(
            t->registry, request->transport_id, request->target_id, required_perm)) {
        t->last_error = ECAT_TRANSFER_ERR_PERMISSION_DENIED;
        release_session(t, owner, acquired_token);
        return ECAT_TRANSFER_DENIED;
    }

    /* 物理边界防越界检查 */
    if (request->direction == ECAT_TRANSFER_DIR_IMPORT &&
        target->type == ECAT_TARGET_TYPE_FIRMWARE) {
        if (request->expected_size > 0U) {
            uint32_t limit = calc_firmware_container_limit(target, t->core);
            if (request->expected_size < ECAT_FOE_EFW_HEADER_MIN_LEN ||
                request->expected_size > limit) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                release_session(t, owner, acquired_token);
                return ECAT_TRANSFER_FAILED;
            }
        }
    } else {
        if (request->expected_size > target->max_size) {
            t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
            release_session(t, owner, acquired_token);
            return ECAT_TRANSFER_FAILED;
        }
    }

    /* 初始化会话参数 */
    t->target = target;
    t->current_transport_id = request->transport_id;
    t->direction = request->direction;
    t->expected_size = request->expected_size;
    t->current_offset = 0U;
    t->erased_watermark = 0U;
    t->active = true;
    t->driver_initialized = false;
    t->last_error = ECAT_TRANSFER_ERR_NONE;
    t->is_async = target->asynchronous;
    t->pending = false;
    t->stage = ECAT_TRANSFER_STAGE_NONE;
    ecat_foe_crc32_init(&t->running_crc32);

    /* 异步驱动能力核验: 严禁假异步，缺少异步后端或 ops 时必须在任何驱动副作用前明确拒绝 */
    if (t->is_async) {
        if (target->type == ECAT_TARGET_TYPE_FIRMWARE) {
            if (t->fw_async_ops == NULL) {
                t->last_error = ECAT_TRANSFER_ERR_CORE_UPGRADE;
                t->active = false;
                release_session(t, owner, acquired_token);
                return ECAT_TRANSFER_FAILED;
            }
        } else {
            if (target->driver.async_ops == NULL ||
                target->driver.async_ops->start == NULL ||
                target->driver.async_ops->poll == NULL) {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_INIT;
                t->active = false;
                release_session(t, owner, acquired_token);
                return ECAT_TRANSFER_FAILED;
            }
        }
    }

    /* 区分方向进行底层准备 */
    if (request->direction == ECAT_TRANSFER_DIR_EXPORT) {
        if (target->driver.io.read == NULL && (!t->is_async || target->driver.async_ops == NULL)) {
            t->last_error = ECAT_TRANSFER_ERR_DRIVER_READ;
            release_session(t, owner, acquired_token);
            return ECAT_TRANSFER_FAILED;
        }

        /* 计算可导出数据量 */
        if (target->driver.get_data_size != NULL) {
            t->total_size = target->driver.get_data_size(target->user_ctx);
            if (t->total_size > target->max_size) {
                t->total_size = target->max_size;
            }
        } else if (request->expected_size > 0U) {
            t->total_size = request->expected_size;
        } else {
            t->total_size = target->max_size;
        }

        /* 只读生命周期安全校验:
         * 1. 允许完全无生命周期的纯 read 驱动 (read_open == NULL && read_close == NULL && io.init == NULL)
         * 2. 安全配对的只读驱动 (read_open != NULL && read_close != NULL)
         * 3. 拒绝任何未安全配对或缺少只读关闭的驱动:
         *    - read_open 与 read_close 仅提供其一 (不配对)
         *    - 需要打开资源 (io.init != NULL) 但缺少 safe read_close (防止资源泄漏或误调 abort 损坏镜像)
         */
        bool has_read_open = (target->driver.read_open != NULL);
        bool has_read_close = (target->driver.read_close != NULL);
        bool has_io_init = (target->driver.io.init != NULL);

        if (has_read_open != has_read_close) {
            t->last_error = ECAT_TRANSFER_ERR_DRIVER_INIT;
            t->active = false;
            t->driver_initialized = false;
            release_session(t, owner, acquired_token);
            return ECAT_TRANSFER_FAILED;
        }

        if (has_io_init && !has_read_close) {
            t->last_error = ECAT_TRANSFER_ERR_DRIVER_INIT;
            t->active = false;
            t->driver_initialized = false;
            release_session(t, owner, acquired_token);
            return ECAT_TRANSFER_FAILED;
        }

        if (has_read_open && has_read_close) {
            int init_rc = target->driver.read_open(target->user_ctx);
            if (init_rc != 0) {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_INIT;
                t->active = false;
                t->driver_initialized = false;
                release_session(t, owner, acquired_token);
                return ECAT_TRANSFER_FAILED;
            }
            t->driver_initialized = true;
        } else {
            /* 纯 read 驱动，无生命周期开销 */
            t->driver_initialized = false;
        }
    } else {
        /* IMPORT 模式 */
        if (target->type == ECAT_TARGET_TYPE_FIRMWARE) {
            if (t->core == NULL) {
                t->last_error = ECAT_TRANSFER_ERR_CORE_UPGRADE;
                t->active = false;
                t->driver_initialized = false;
                release_session(t, owner, acquired_token);
                return ECAT_TRANSFER_FAILED;
            }

            if (request->target_id < 1U || request->target_id > ECAT_FOE_TARGET_COUNT) {
                t->last_error = ECAT_TRANSFER_ERR_TARGET_NOT_FOUND;
                t->active = false;
                t->driver_initialized = false;
                release_session(t, owner, acquired_token);
                return ECAT_TRANSFER_FAILED;
            }

            uint32_t target_idx = (uint32_t)request->target_id - 1U;
            if (!t->core->cfg.targets[target_idx].enabled) {
                t->last_error = ECAT_TRANSFER_ERR_TARGET_NOT_FOUND;
                t->active = false;
                t->driver_initialized = false;
                release_session(t, owner, acquired_token);
                return ECAT_TRANSFER_FAILED;
            }

            /* 校验并绑定底层驱动映射: 验证上下文与所有非空驱动回调一致性 */
            const ecat_foe_target_driver_t *cdrv = t->core->cfg.targets[target_idx].driver;
            const ecat_foe_target_driver_t *rdrv = &target->driver.io;

            if (cdrv == NULL) {
                if (rdrv->write == NULL) {
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_WRITE;
                    t->active = false;
                    t->driver_initialized = false;
                    release_session(t, owner, acquired_token);
                    return ECAT_TRANSFER_FAILED;
                }
                t->core->cfg.targets[target_idx].driver = rdrv;
                t->core->cfg.targets[target_idx].user_ctx = target->user_ctx;
            } else {
                if (target->user_ctx != t->core->cfg.targets[target_idx].user_ctx ||
                    rdrv->write == NULL ||
                    cdrv->write != rdrv->write ||
                    cdrv->init != rdrv->init ||
                    cdrv->invalidate != rdrv->invalidate ||
                    cdrv->erase_to != rdrv->erase_to ||
                    cdrv->read != rdrv->read ||
                    cdrv->finalize != rdrv->finalize ||
                    cdrv->abort != rdrv->abort ||
                    cdrv->finalize_with_version_floor != rdrv->finalize_with_version_floor) {
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_WRITE;
                    t->active = false;
                    t->driver_initialized = false;
                    release_session(t, owner, acquired_token);
                    return ECAT_TRANSFER_FAILED;
                }
            }

            /* 统一容量约束: 物理上限以 registry 与 core 中的更严格者为准，暂存原容量防止永久污染 */
            uint32_t effective_cap = target->max_size;
            if (t->core->cfg.targets[target_idx].max_payload_size > 0U &&
                t->core->cfg.targets[target_idx].max_payload_size < effective_cap) {
                effective_cap = t->core->cfg.targets[target_idx].max_payload_size;
            }

            t->saved_core_max_payload = t->core->cfg.targets[target_idx].max_payload_size;
            t->saved_core_target_idx = target_idx;
            t->core_cap_overridden = true;
            t->core->cfg.targets[target_idx].max_payload_size = effective_cap;

            t->total_size = request->expected_size;

            ecat_upgrade_request_t core_req;
            core_req.target_id = request->target_id;
            core_req.fw_version = request->fw_version;

            if (t->core->state == ECAT_FOE_STATE_SUCCESS || t->core->state == ECAT_FOE_STATE_ERROR) {
                ecat_foe_core_on_abort(t->core);
            }

            if (ecat_foe_core_begin_request(t->core, &core_req) != ECAT_FOE_CORE_OK) {
                t->last_error = ECAT_TRANSFER_ERR_CORE_UPGRADE;
                t->active = false;
                t->driver_initialized = false;
                release_session(t, owner, acquired_token);
                return ECAT_TRANSFER_FAILED;
            }
            t->core_started = true;
        } else {
            /* DATA 模式 */
            t->total_size = (request->expected_size > 0U) ? request->expected_size : target->max_size;

            /* DATA 模式驱动初始化与失效化生命周期 (同步与异步统一执行) */
            if (target->driver.io.init != NULL) {
                if (target->driver.io.init(target->user_ctx) != 0) {
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_INIT;
                    t->active = false;
                    t->driver_initialized = false;
                    release_session(t, owner, acquired_token);
                    return ECAT_TRANSFER_FAILED;
                }
            }
            t->driver_initialized = true;

            if (target->driver.io.invalidate != NULL) {
                if (target->driver.io.invalidate(target->user_ctx) != 0) {
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_INVALIDATE;
                    if (t->target->driver.async_ops && t->target->driver.async_ops->abort) {
                        t->target->driver.async_ops->abort(t->target->user_ctx);
                    }
                    if (target->driver.io.abort != NULL) {
                        target->driver.io.abort(target->user_ctx);
                    }
                    t->active = false;
                    t->driver_initialized = false;
                    release_session(t, owner, acquired_token);
                    return ECAT_TRANSFER_FAILED;
                }
            }
        }
    }

    *token = acquired_token;
    return ECAT_TRANSFER_COMPLETE;
}

ecat_transfer_result_t ecat_transfer_read(
    ecat_transfer_t *t,
    const void *owner,
    uint64_t token,
    uint8_t *out_buf,
    size_t buf_capacity,
    size_t *actual_read)
{
    if (!is_owned(t, owner, token)) {
        return ECAT_TRANSFER_STALE;
    }

    if (t->pending) {
        return ECAT_TRANSFER_BUSY;
    }

    if (t->direction != ECAT_TRANSFER_DIR_EXPORT || out_buf == NULL ||
        actual_read == NULL || buf_capacity == 0U) {
        return ECAT_TRANSFER_INVALID;
    }

    *actual_read = 0U;

    /* 检查是否已达到导出终点 */
    if (t->current_offset >= t->total_size) {
        return ECAT_TRANSFER_COMPLETE;
    }

    uint32_t remaining = t->total_size - t->current_offset;
    size_t to_read_sz = ((size_t)remaining < buf_capacity) ? (size_t)remaining : buf_capacity;
    uint32_t to_read = (uint32_t)to_read_sz;

    if (t->is_async && t->target->type == ECAT_TARGET_TYPE_DATA) {
        if (t->target->driver.async_ops == NULL || t->target->driver.async_ops->start == NULL) {
            t->last_error = ECAT_TRANSFER_ERR_DRIVER_READ;
            return ECAT_TRANSFER_FAILED;
        }

        memset(&t->async_data_op, 0, sizeof(t->async_data_op));
        t->async_data_op.kind = ECAT_ASYNC_DATA_OP_READ;
        t->async_data_op.offset = t->current_offset;
        t->async_data_op.length = to_read;
        t->async_data_op.buffer = out_buf;

        t->user_read_out_buf = out_buf;
        t->user_read_actual = actual_read;
        t->pending_length = to_read;

        ecat_async_op_result_t res = t->target->driver.async_ops->start(t->target->user_ctx, &t->async_data_op);
        if (res == ECAT_ASYNC_OP_PENDING) {
            t->pending = true;
            t->stage = ECAT_TRANSFER_STAGE_READ;
            return ECAT_TRANSFER_PENDING;
        } else if (res == ECAT_ASYNC_OP_COMPLETE) {
            size_t actual = t->async_data_op.actual_length;
            if (actual > (size_t)to_read) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                if (t->target->driver.read_close != NULL) {
                    t->target->driver.read_close(t->target->user_ctx);
                }
                t->driver_initialized = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
            t->current_offset += (uint32_t)actual;
            *actual_read = actual;
            if (actual > 0U) {
                ecat_foe_crc32_update(&t->running_crc32, out_buf, (uint32_t)actual);
            }
            if (t->current_offset >= t->total_size) {
                return ECAT_TRANSFER_COMPLETE;
            }
            return ECAT_TRANSFER_CONTINUE;
        } else {
            t->last_error = ECAT_TRANSFER_ERR_DRIVER_READ;
            if (t->target->driver.read_close != NULL) {
                t->target->driver.read_close(t->target->user_ctx);
            }
            t->driver_initialized = false;
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }
    }

    if (t->target->driver.io.read == NULL) {
        t->last_error = ECAT_TRANSFER_ERR_DRIVER_READ;
        return ECAT_TRANSFER_FAILED;
    }

    int rc = t->target->driver.io.read(
        t->current_offset, out_buf, to_read, t->target->user_ctx);
    if (rc != 0) {
        t->last_error = ECAT_TRANSFER_ERR_DRIVER_READ;
        if (t->driver_initialized && t->target != NULL) {
            if (t->direction == ECAT_TRANSFER_DIR_EXPORT) {
                if (t->target->driver.read_close != NULL) {
                    t->target->driver.read_close(t->target->user_ctx);
                }
            } else if (t->target->driver.io.abort != NULL) {
                t->target->driver.io.abort(t->target->user_ctx);
            }
        }
        t->driver_initialized = false;
        t->active = false;
        release_session(t, owner, token);
        return ECAT_TRANSFER_FAILED;
    }

    t->current_offset += to_read;
    ecat_foe_crc32_update(&t->running_crc32, out_buf, to_read);
    *actual_read = to_read;

    if (t->current_offset >= t->total_size) {
        return ECAT_TRANSFER_COMPLETE;
    }

    return ECAT_TRANSFER_CONTINUE;
}

ecat_transfer_result_t ecat_transfer_write(
    ecat_transfer_t *t,
    const void *owner,
    uint64_t token,
    const uint8_t *data,
    size_t length)
{
    if (!is_owned(t, owner, token)) {
        return ECAT_TRANSFER_STALE;
    }

    if (t->pending) {
        return ECAT_TRANSFER_BUSY;
    }

    if (t->direction != ECAT_TRANSFER_DIR_IMPORT) {
        return ECAT_TRANSFER_INVALID;
    }

    if (length == 0U) {
        return ECAT_TRANSFER_COMPLETE;
    }

    if (data == NULL) {
        return ECAT_TRANSFER_INVALID;
    }

    if (t->target->type == ECAT_TARGET_TYPE_FIRMWARE) {
        if (t->core == NULL) {
            t->last_error = ECAT_TRANSFER_ERR_CORE_UPGRADE;
            return ECAT_TRANSFER_FAILED;
        }

        /* 固件容器物理上限有界推导: 统一调用 calc_firmware_container_limit，防止加法溢出与扩展头拒绝 */
        uint32_t limit = calc_firmware_container_limit(t->target, t->core);
        uint32_t fw_cap = (t->total_size > 0U) ? t->total_size : limit;

        /* 先用 size_t 与减法防环绕拒绝超界长度，保证 32 位与 64 位平台均绝不溢出 */
        if (length > (size_t)UINT32_MAX ||
            t->current_offset > fw_cap ||
            length > (size_t)(fw_cap - t->current_offset)) {
            t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
            if (t->is_async && t->fw_async_ops != NULL && t->fw_async_ops->abort != NULL) {
                t->fw_async_ops->abort(t->fw_async_ctx);
            } else if (t->core != NULL) {
                ecat_foe_core_on_abort(t->core);
            }
            t->core_started = false;
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }

        if (t->is_async && t->fw_async_ops != NULL) {
            if (t->staging_buf == NULL || length > t->staging_capacity) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                if (t->fw_async_ops->abort) t->fw_async_ops->abort(t->fw_async_ctx);
                t->core_started = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
            memcpy(t->staging_buf, data, length);
            ecat_upgrade_async_result_t res = t->fw_async_ops->start(t->fw_async_ctx, t->staging_buf, length, true);
            if (res == ECAT_UPGRADE_ASYNC_PENDING) {
                t->pending = true;
                t->stage = ECAT_TRANSFER_STAGE_WRITE;
                t->pending_length = length;
                return ECAT_TRANSFER_PENDING;
            } else if (res == ECAT_UPGRADE_ASYNC_COMPLETE) {
                t->current_offset += (uint32_t)length;
                ecat_foe_crc32_update(&t->running_crc32, t->staging_buf, (uint32_t)length);
                return ECAT_TRANSFER_COMPLETE;
            } else {
                t->last_error = ECAT_TRANSFER_ERR_CORE_UPGRADE;
                if (t->fw_async_ops->abort) t->fw_async_ops->abort(t->fw_async_ctx);
                t->core_started = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
        }

        uint32_t len32 = (uint32_t)length;
        if (ecat_foe_core_on_data(t->core, data, len32) != 0) {
            if (t->core->last_error == ECAT_FOE_CORE_ERR_PAYLOAD_LIMIT_EXCEEDED) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
            } else {
                t->last_error = ECAT_TRANSFER_ERR_CORE_UPGRADE;
            }
            t->core_started = false;
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }

        /* 持续执行注册表有效负载物理容量核验 */
        if (t->core->header_complete) {
            if (t->core->hdr.payload_length > t->target->max_size ||
                t->core->written_offset > t->target->max_size) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                ecat_foe_core_on_abort(t->core);
                t->core_started = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
        }

        t->current_offset += len32;
        ecat_foe_crc32_update(&t->running_crc32, data, len32);
    } else {
        /* DATA 普通数据流导入: 先用 size_t 与减法防环绕拒绝超界长度 */
        uint32_t write_limit = (t->expected_size > 0U) ? t->expected_size : t->target->max_size;
        if (length > (size_t)UINT32_MAX ||
            t->current_offset > write_limit ||
            length > (size_t)(write_limit - t->current_offset)) {
            t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
            if (t->is_async && t->target->driver.async_ops && t->target->driver.async_ops->abort) {
                t->target->driver.async_ops->abort(t->target->user_ctx);
            } else if (t->driver_initialized && t->target->driver.io.abort != NULL) {
                t->target->driver.io.abort(t->target->user_ctx);
            }
            t->driver_initialized = false;
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }

        if (t->is_async && t->target->type == ECAT_TARGET_TYPE_DATA) {
            if (t->staging_buf == NULL || length > t->staging_capacity) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                if (t->target->driver.async_ops && t->target->driver.async_ops->abort) {
                    t->target->driver.async_ops->abort(t->target->user_ctx);
                }
                if (t->driver_initialized && t->target->driver.io.abort != NULL) {
                    t->target->driver.io.abort(t->target->user_ctx);
                }
                t->driver_initialized = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }

            /* 异步 DATA 写入前按需擦除 */
            uint32_t len32 = (uint32_t)length;
            uint32_t need_end = t->current_offset + len32;
            if (need_end > t->erased_watermark) {
                uint32_t actual_erased = 0U;
                if (t->target->driver.io.erase_to != NULL) {
                    int erase_rc = t->target->driver.io.erase_to(
                        need_end, &actual_erased, t->target->user_ctx);
                    if (erase_rc != 0 || actual_erased < need_end) {
                        t->last_error = ECAT_TRANSFER_ERR_DRIVER_ERASE;
                        if (t->target->driver.async_ops && t->target->driver.async_ops->abort) {
                            t->target->driver.async_ops->abort(t->target->user_ctx);
                        }
                        if (t->driver_initialized && t->target->driver.io.abort != NULL) {
                            t->target->driver.io.abort(t->target->user_ctx);
                        }
                        t->driver_initialized = false;
                        t->active = false;
                        release_session(t, owner, token);
                        return ECAT_TRANSFER_FAILED;
                    }
                    t->erased_watermark = actual_erased;
                }
            }

            memcpy(t->staging_buf, data, length);
            memset(&t->async_data_op, 0, sizeof(t->async_data_op));
            t->async_data_op.kind = ECAT_ASYNC_DATA_OP_WRITE;
            t->async_data_op.offset = t->current_offset;
            t->async_data_op.length = (uint32_t)length;
            t->async_data_op.buffer = t->staging_buf;

            ecat_async_op_result_t res = t->target->driver.async_ops->start(t->target->user_ctx, &t->async_data_op);
            if (res == ECAT_ASYNC_OP_PENDING) {
                t->pending = true;
                t->stage = ECAT_TRANSFER_STAGE_WRITE;
                t->pending_length = length;
                return ECAT_TRANSFER_PENDING;
            } else if (res == ECAT_ASYNC_OP_COMPLETE) {
                size_t actual = t->async_data_op.actual_length;
                if (actual > length) {
                    t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                    if (t->target->driver.async_ops && t->target->driver.async_ops->abort) {
                        t->target->driver.async_ops->abort(t->target->user_ctx);
                    }
                    if (t->driver_initialized && t->target->driver.io.abort != NULL) {
                        t->target->driver.io.abort(t->target->user_ctx);
                    }
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
                if (actual < length) {
                    /* 原子完整写入契约: 短写或 0 字节写入属于非法驱动行为，必须明确报错以防止数据截断丢失 */
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_WRITE;
                    if (t->target->driver.async_ops && t->target->driver.async_ops->abort) {
                        t->target->driver.async_ops->abort(t->target->user_ctx);
                    }
                    if (t->driver_initialized && t->target->driver.io.abort != NULL) {
                        t->target->driver.io.abort(t->target->user_ctx);
                    }
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
                t->current_offset += (uint32_t)actual;
                ecat_foe_crc32_update(&t->running_crc32, t->staging_buf, (uint32_t)actual);
                return ECAT_TRANSFER_COMPLETE;
            } else {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_WRITE;
                if (t->target->driver.async_ops->abort) {
                    t->target->driver.async_ops->abort(t->target->user_ctx);
                }
                t->driver_initialized = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
        }

        uint32_t len32 = (uint32_t)length;
        /* 按需擦除 */
        uint32_t need_end = t->current_offset + len32;
        if (need_end > t->erased_watermark) {
            uint32_t actual_erased = 0U;
            if (t->target->driver.io.erase_to != NULL) {
                int erase_rc = t->target->driver.io.erase_to(
                    need_end, &actual_erased, t->target->user_ctx);
                if (erase_rc != 0 || actual_erased < need_end) {
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_ERASE;
                    if (t->driver_initialized && t->target->driver.io.abort != NULL) {
                        t->target->driver.io.abort(t->target->user_ctx);
                    }
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
                t->erased_watermark = actual_erased;
            }
        }

        /* 写入介质 */
        if (t->target->driver.io.write == NULL) {
            t->last_error = ECAT_TRANSFER_ERR_DRIVER_WRITE;
            if (t->driver_initialized && t->target->driver.io.abort != NULL) {
                t->target->driver.io.abort(t->target->user_ctx);
            }
            t->driver_initialized = false;
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }

        int write_rc = t->target->driver.io.write(
            t->current_offset, data, len32, t->target->user_ctx);
        if (write_rc != 0) {
            t->last_error = ECAT_TRANSFER_ERR_DRIVER_WRITE;
            if (t->driver_initialized && t->target->driver.io.abort != NULL) {
                t->target->driver.io.abort(t->target->user_ctx);
            }
            t->driver_initialized = false;
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }

        t->current_offset += len32;
        ecat_foe_crc32_update(&t->running_crc32, data, len32);
    }

    return ECAT_TRANSFER_COMPLETE;
}

ecat_transfer_result_t ecat_transfer_finish(
    ecat_transfer_t *t,
    const void *owner,
    uint64_t token)
{
    if (!is_owned(t, owner, token)) {
        return ECAT_TRANSFER_STALE;
    }

    if (t->pending) {
        return ECAT_TRANSFER_BUSY;
    }

    if (t->is_async) {
        if (t->target->type == ECAT_TARGET_TYPE_FIRMWARE) {
            if (t->direction == ECAT_TRANSFER_DIR_EXPORT) {
                if (t->driver_initialized && t->target->driver.read_close != NULL) {
                    t->target->driver.read_close(t->target->user_ctx);
                }
                t->core_started = false;
                t->driver_initialized = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_COMPLETE;
            }

            /* 异步固件导入核验 expected_size */
            if (t->total_size > 0U && t->current_offset != t->total_size) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                if (t->fw_async_ops != NULL && t->fw_async_ops->abort != NULL) {
                    t->fw_async_ops->abort(t->fw_async_ctx);
                }
                if (t->core_started && t->core != NULL) {
                    ecat_foe_core_on_abort(t->core);
                }
                t->core_started = false;
                t->driver_initialized = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }

            if (t->fw_async_ops != NULL) {
                ecat_upgrade_async_result_t res = t->fw_async_ops->start(t->fw_async_ctx, NULL, 0U, false);
                if (res == ECAT_UPGRADE_ASYNC_PENDING) {
                    t->pending = true;
                    t->stage = ECAT_TRANSFER_STAGE_FINALIZE;
                    return ECAT_TRANSFER_PENDING;
                } else if (res == ECAT_UPGRADE_ASYNC_COMPLETE) {
                    t->core_started = false;
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_COMPLETE;
                } else {
                    t->last_error = ECAT_TRANSFER_ERR_CORE_UPGRADE;
                    if (t->fw_async_ops->abort) t->fw_async_ops->abort(t->fw_async_ctx);
                    t->core_started = false;
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
            }
        } else {
            /* DATA 模式异步 finish */
            if (t->direction == ECAT_TRANSFER_DIR_IMPORT) {
                if (t->expected_size > 0U && t->current_offset != t->expected_size) {
                    t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                    if (t->target->driver.async_ops && t->target->driver.async_ops->abort) {
                        t->target->driver.async_ops->abort(t->target->user_ctx);
                    }
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }

                memset(&t->async_data_op, 0, sizeof(t->async_data_op));
                t->async_data_op.kind = ECAT_ASYNC_DATA_OP_FINALIZE;
                ecat_async_op_result_t res = t->target->driver.async_ops->start(t->target->user_ctx, &t->async_data_op);
                if (res == ECAT_ASYNC_OP_PENDING) {
                    t->pending = true;
                    t->stage = ECAT_TRANSFER_STAGE_FINALIZE;
                    return ECAT_TRANSFER_PENDING;
                } else if (res == ECAT_ASYNC_OP_COMPLETE) {
                    if (t->target->driver.io.finalize != NULL) {
                        if (t->target->driver.io.finalize(t->target->user_ctx) != 0) {
                            t->last_error = ECAT_TRANSFER_ERR_DRIVER_FINALIZE;
                            if (t->target->driver.async_ops->abort) {
                                t->target->driver.async_ops->abort(t->target->user_ctx);
                            }
                            if (t->target->driver.io.abort != NULL) {
                                t->target->driver.io.abort(t->target->user_ctx);
                            }
                            t->driver_initialized = false;
                            t->active = false;
                            release_session(t, owner, token);
                            return ECAT_TRANSFER_FAILED;
                        }
                    }
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_COMPLETE;
                } else {
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_FINALIZE;
                    if (t->target->driver.async_ops->abort) {
                        t->target->driver.async_ops->abort(t->target->user_ctx);
                    }
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
            } else {
                /* EXPORT 模式: 正常关闭只读会话，绝不调用 finalize 或写 abort */
                if (t->target->driver.read_close != NULL) {
                    t->target->driver.read_close(t->target->user_ctx);
                }
                t->driver_initialized = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_COMPLETE;
            }
        }
    }

    if (t->direction == ECAT_TRANSFER_DIR_EXPORT) {
        /* EXPORT 模式: 只读传输结束，仅调用 read_close，严禁调用写提交 io.finalize */
        if (t->driver_initialized && t->target != NULL && t->target->driver.read_close != NULL) {
            t->target->driver.read_close(t->target->user_ctx);
        }
    } else {
        /* IMPORT 模式 */
        if (t->target->type == ECAT_TARGET_TYPE_FIRMWARE) {
            if (t->total_size > 0U && t->current_offset != t->total_size) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                if (t->core != NULL) {
                    ecat_foe_core_on_abort(t->core);
                }
                t->core_started = false;
                t->driver_initialized = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
            if (t->core != NULL && ecat_foe_core_on_end(t->core) != 0) {
                t->last_error = ECAT_TRANSFER_ERR_CORE_UPGRADE;
                t->core_started = false;
                t->driver_initialized = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
        } else {
            /* DATA 模式: 若声明 expected_size 则必须严格完全对齐 */
            if (t->expected_size > 0U && t->current_offset != t->expected_size) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                if (t->driver_initialized && t->target->driver.io.abort != NULL) {
                    t->target->driver.io.abort(t->target->user_ctx);
                }
                t->core_started = false;
                t->driver_initialized = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }

            if (t->target->driver.io.finalize != NULL) {
                if (t->target->driver.io.finalize(t->target->user_ctx) != 0) {
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_FINALIZE;
                    if (t->driver_initialized && t->target->driver.io.abort != NULL) {
                        t->target->driver.io.abort(t->target->user_ctx);
                    }
                    t->core_started = false;
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
            }
        }
    }

    t->core_started = false;
    t->driver_initialized = false;
    t->active = false;
    release_session(t, owner, token);
    return ECAT_TRANSFER_COMPLETE;
}

ecat_transfer_result_t ecat_transfer_cancel(
    ecat_transfer_t *t,
    const void *owner,
    uint64_t token)
{
    if (!is_owned(t, owner, token)) {
        return ECAT_TRANSFER_STALE;
    }

    bool async_aborted = false;
    /* 若处于 pending，先中止后台异步操作并确保硬件停用暂存区 */
    if (t->pending) {
        if (t->target != NULL && t->target->type == ECAT_TARGET_TYPE_FIRMWARE) {
            if (t->fw_async_ops != NULL && t->fw_async_ops->abort != NULL) {
                t->fw_async_ops->abort(t->fw_async_ctx);
                async_aborted = true;
            }
        } else if (t->target != NULL && t->target->driver.async_ops != NULL) {
            if (t->target->driver.async_ops->abort != NULL) {
                t->target->driver.async_ops->abort(t->target->user_ctx);
                async_aborted = true;
            }
        }
        t->pending = false;
        t->stage = ECAT_TRANSFER_STAGE_NONE;
    }

    if (t->core_started && t->core != NULL) {
        ecat_foe_core_on_abort(t->core);
    } else if (t->driver_initialized && t->target != NULL) {
        if (t->direction == ECAT_TRANSFER_DIR_EXPORT && t->target->driver.read_close != NULL) {
            t->target->driver.read_close(t->target->user_ctx);
        } else if (!async_aborted && t->target->driver.io.abort != NULL) {
            t->target->driver.io.abort(t->target->user_ctx);
        }
    }

    t->core_started = false;
    t->driver_initialized = false;
    t->active = false;
    release_session(t, owner, token);
    return ECAT_TRANSFER_COMPLETE;
}

ecat_transfer_result_t ecat_transfer_poll(
    ecat_transfer_t *t,
    const void *owner,
    uint64_t token)
{
    if (!is_owned(t, owner, token)) {
        return ECAT_TRANSFER_STALE;
    }

    if (!t->pending) {
        return ECAT_TRANSFER_INVALID;
    }

    if (t->target->type == ECAT_TARGET_TYPE_FIRMWARE) {
        if (t->fw_async_ops == NULL || t->fw_async_ops->poll == NULL) {
            return ECAT_TRANSFER_INVALID;
        }

        ecat_upgrade_async_result_t res = t->fw_async_ops->poll(t->fw_async_ctx);
        if (res == ECAT_UPGRADE_ASYNC_PENDING) {
            return ECAT_TRANSFER_PENDING;
        } else if (res == ECAT_UPGRADE_ASYNC_COMPLETE) {
            t->pending = false;
            if (t->stage == ECAT_TRANSFER_STAGE_FINALIZE) {
                t->active = false;
                t->core_started = false;
                t->driver_initialized = false;
                t->stage = ECAT_TRANSFER_STAGE_NONE;
                release_session(t, owner, token);
                return ECAT_TRANSFER_COMPLETE;
            } else {
                t->current_offset += (uint32_t)t->pending_length;
                if (t->staging_buf != NULL && t->pending_length <= t->staging_capacity) {
                    ecat_foe_crc32_update(&t->running_crc32, t->staging_buf, (uint32_t)t->pending_length);
                }
                t->stage = ECAT_TRANSFER_STAGE_NONE;
                return ECAT_TRANSFER_COMPLETE;
            }
        } else {
            t->pending = false;
            t->stage = ECAT_TRANSFER_STAGE_NONE;
            t->last_error = ECAT_TRANSFER_ERR_CORE_UPGRADE;
            if (t->fw_async_ops->abort != NULL) {
                t->fw_async_ops->abort(t->fw_async_ctx);
            }
            t->core_started = false;
            t->driver_initialized = false;
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }
    } else {
        /* DATA 目标异步轮询 */
        if (t->target->driver.async_ops == NULL || t->target->driver.async_ops->poll == NULL) {
            return ECAT_TRANSFER_INVALID;
        }

        ecat_async_op_result_t res = t->target->driver.async_ops->poll(t->target->user_ctx, &t->async_data_op);
        if (res == ECAT_ASYNC_OP_PENDING) {
            return ECAT_TRANSFER_PENDING;
        } else if (res == ECAT_ASYNC_OP_COMPLETE) {
            t->pending = false;
            if (t->stage == ECAT_TRANSFER_STAGE_WRITE) {
                size_t actual = t->async_data_op.actual_length;
                if (actual > (size_t)t->pending_length) {
                    t->stage = ECAT_TRANSFER_STAGE_NONE;
                    t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                    if (t->target->driver.async_ops && t->target->driver.async_ops->abort) {
                        t->target->driver.async_ops->abort(t->target->user_ctx);
                    }
                    if (t->driver_initialized && t->target->driver.io.abort != NULL) {
                        t->target->driver.io.abort(t->target->user_ctx);
                    }
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
                if (actual < (size_t)t->pending_length) {
                    /* 轮询完成时短写或 0 字节写入同样必须明确拒绝并安全释放，杜绝静默截断 */
                    t->stage = ECAT_TRANSFER_STAGE_NONE;
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_WRITE;
                    if (t->target->driver.async_ops && t->target->driver.async_ops->abort) {
                        t->target->driver.async_ops->abort(t->target->user_ctx);
                    }
                    if (t->driver_initialized && t->target->driver.io.abort != NULL) {
                        t->target->driver.io.abort(t->target->user_ctx);
                    }
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
                t->current_offset += (uint32_t)actual;
                ecat_foe_crc32_update(&t->running_crc32, t->staging_buf, (uint32_t)actual);
                t->stage = ECAT_TRANSFER_STAGE_NONE;
                return ECAT_TRANSFER_COMPLETE;
            } else if (t->stage == ECAT_TRANSFER_STAGE_READ) {
                size_t actual = t->async_data_op.actual_length;
                if (actual > (size_t)t->pending_length) {
                    t->stage = ECAT_TRANSFER_STAGE_NONE;
                    t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                    if (t->target->driver.read_close != NULL) {
                        t->target->driver.read_close(t->target->user_ctx);
                    }
                    t->driver_initialized = false;
                    t->active = false;
                    release_session(t, owner, token);
                    return ECAT_TRANSFER_FAILED;
                }
                t->current_offset += (uint32_t)actual;
                if (t->user_read_actual != NULL) {
                    *t->user_read_actual = actual;
                }
                if (actual > 0U && t->user_read_out_buf != NULL) {
                    ecat_foe_crc32_update(&t->running_crc32, t->user_read_out_buf, (uint32_t)actual);
                }
                t->stage = ECAT_TRANSFER_STAGE_NONE;
                if (t->current_offset >= t->total_size) {
                    return ECAT_TRANSFER_COMPLETE;
                }
                return ECAT_TRANSFER_CONTINUE;
            } else if (t->stage == ECAT_TRANSFER_STAGE_FINALIZE || t->stage == ECAT_TRANSFER_STAGE_CLOSING) {
                if (t->stage == ECAT_TRANSFER_STAGE_FINALIZE && t->target->driver.io.finalize != NULL) {
                    if (t->target->driver.io.finalize(t->target->user_ctx) != 0) {
                        t->last_error = ECAT_TRANSFER_ERR_DRIVER_FINALIZE;
                        if (t->target->driver.async_ops->abort) {
                            t->target->driver.async_ops->abort(t->target->user_ctx);
                        }
                        if (t->target->driver.io.abort != NULL) {
                            t->target->driver.io.abort(t->target->user_ctx);
                        }
                        t->driver_initialized = false;
                        t->active = false;
                        t->stage = ECAT_TRANSFER_STAGE_NONE;
                        release_session(t, owner, token);
                        return ECAT_TRANSFER_FAILED;
                    }
                }
                t->active = false;
                t->stage = ECAT_TRANSFER_STAGE_NONE;
                t->driver_initialized = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_COMPLETE;
            } else {
                t->stage = ECAT_TRANSFER_STAGE_NONE;
                return ECAT_TRANSFER_COMPLETE;
            }
        } else {
            t->pending = false;
            ecat_transfer_stage_t failed_stage = t->stage;
            t->stage = ECAT_TRANSFER_STAGE_NONE;
            t->last_error = (failed_stage == ECAT_TRANSFER_STAGE_READ) ? ECAT_TRANSFER_ERR_DRIVER_READ :
                            ((failed_stage == ECAT_TRANSFER_STAGE_FINALIZE) ? ECAT_TRANSFER_ERR_DRIVER_FINALIZE : ECAT_TRANSFER_ERR_DRIVER_WRITE);
            if (t->direction == ECAT_TRANSFER_DIR_EXPORT) {
                if (t->target->driver.read_close != NULL) {
                    t->target->driver.read_close(t->target->user_ctx);
                }
            } else {
                if (t->target->driver.async_ops->abort != NULL) {
                    t->target->driver.async_ops->abort(t->target->user_ctx);
                }
            }
            t->driver_initialized = false;
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }
    }
}

uint32_t ecat_transfer_get_crc32(const ecat_transfer_t *t)
{
    if (t == NULL) {
        return 0U;
    }
    return ecat_foe_crc32_final(&t->running_crc32);
}

uint32_t ecat_transfer_get_bytes_transferred(const ecat_transfer_t *t)
{
    if (t == NULL) {
        return 0U;
    }
    return t->current_offset;
}

uint32_t ecat_transfer_get_total_export_size(const ecat_transfer_t *t)
{
    if (t == NULL) {
        return 0U;
    }
    return t->total_size;
}

ecat_transfer_err_t ecat_transfer_get_last_error(const ecat_transfer_t *t)
{
    if (t == NULL) {
        return ECAT_TRANSFER_ERR_INVALID_ARG;
    }
    return t->last_error;
}
