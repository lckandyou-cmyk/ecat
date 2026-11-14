/**
 * @file ecat_transfer.c
 * @brief 嵌入式多通道双向传输管理器实现
 */

#include "ecat_transfer.h"
#include "ecat_foe_efw.h"
#include <string.h>

int ecat_transfer_init(ecat_transfer_t *t, ecat_target_registry_t *reg, ecat_foe_core_t *core)
{
    if (t == NULL || reg == NULL) {
        return -1;
    }

    memset(t, 0, sizeof(*t));
    t->registry = reg;
    t->core = core;
    t->last_error = ECAT_TRANSFER_ERR_NONE;

    if (core != NULL) {
        core->registry = reg;
        core->acl_check = (bool (*)(const void *, uint16_t, uint16_t, uint8_t))ecat_target_registry_check_permission;
        if (core->shared_session == NULL) {
            core->shared_session = &t->session;
        }
        t->session_ptr = (ecat_upgrade_session_t *)core->shared_session;
    } else {
        t->session_ptr = &t->session;
    }

    return 0;
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
    ecat_upgrade_session_t *s = get_transfer_session(t);
    if (s != NULL) {
        (void)ecat_upgrade_session_release(s, owner, token);
    }
}

static bool is_owned(const ecat_transfer_t *t, const void *owner, uint64_t token)
{
    return (t != NULL && ecat_upgrade_session_owns(get_transfer_session_const(t), owner, token));
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
        ecat_upgrade_session_acquire(get_transfer_session(t), owner, &acquired_token);

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
            if (request->expected_size < 148U ||
                request->expected_size > (target->max_size + 148U + 7U)) {
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
    t->current_offset = 0U;
    t->erased_watermark = 0U;
    t->active = true;
    t->driver_initialized = false;
    t->last_error = ECAT_TRANSFER_ERR_NONE;
    ecat_foe_crc32_init(&t->running_crc32);

    /* 区分方向进行底层准备 */
    if (request->direction == ECAT_TRANSFER_DIR_EXPORT) {
        if (target->driver.io.read == NULL) {
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

        /* 驱动只读打开或 init 初始化硬件外设 */
        int init_rc = 0;
        if (target->driver.read_open != NULL) {
            init_rc = target->driver.read_open(target->user_ctx);
        } else if (target->driver.io.init != NULL) {
            init_rc = target->driver.io.init(target->user_ctx);
        }

        if (init_rc != 0) {
            t->last_error = ECAT_TRANSFER_ERR_DRIVER_INIT;
            t->active = false;
            t->driver_initialized = false;
            release_session(t, owner, acquired_token);
            return ECAT_TRANSFER_FAILED;
        }
        t->driver_initialized = true;
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

            /* 校验并绑定底层驱动映射 */
            if (t->core->cfg.targets[target_idx].driver == NULL) {
                if (target->driver.io.write != NULL) {
                    t->core->cfg.targets[target_idx].driver = &target->driver.io;
                } else {
                    t->last_error = ECAT_TRANSFER_ERR_DRIVER_WRITE;
                    t->active = false;
                    t->driver_initialized = false;
                    release_session(t, owner, acquired_token);
                    return ECAT_TRANSFER_FAILED;
                }
            } else if (target->driver.io.write != NULL &&
                       t->core->cfg.targets[target_idx].driver != &target->driver.io &&
                       t->core->cfg.targets[target_idx].driver->write != target->driver.io.write) {
                t->last_error = ECAT_TRANSFER_ERR_DRIVER_WRITE;
                t->active = false;
                t->driver_initialized = false;
                release_session(t, owner, acquired_token);
                return ECAT_TRANSFER_FAILED;
            }

            /* 统一容量约束: 物理上限以 registry 与 core 中的更严格者为准并双向对齐 */
            uint32_t effective_cap = target->max_size;
            if (t->core->cfg.targets[target_idx].max_payload_size > 0U &&
                t->core->cfg.targets[target_idx].max_payload_size < effective_cap) {
                effective_cap = t->core->cfg.targets[target_idx].max_payload_size;
            }
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
    uint32_t to_read = (remaining < (uint32_t)buf_capacity) ? remaining : (uint32_t)buf_capacity;

    if (t->target->driver.io.read == NULL) {
        t->last_error = ECAT_TRANSFER_ERR_DRIVER_READ;
        return ECAT_TRANSFER_FAILED;
    }

    int rc = t->target->driver.io.read(
        t->current_offset, out_buf, to_read, t->target->user_ctx);
    if (rc != 0) {
        t->last_error = ECAT_TRANSFER_ERR_DRIVER_READ;
        if (t->driver_initialized) {
            if (t->target->driver.read_close != NULL) {
                t->target->driver.read_close(t->target->user_ctx);
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

        /* 若声明了 expected_size，持续执行总包长度边界检查 */
        if (t->total_size > 0U) {
            if ((uint32_t)length > (t->total_size - t->current_offset)) {
                t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
                ecat_foe_core_on_abort(t->core);
                t->core_started = false;
                t->active = false;
                release_session(t, owner, token);
                return ECAT_TRANSFER_FAILED;
            }
        }

        if (ecat_foe_core_on_data(t->core, data, length) != 0) {
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

        t->current_offset += (uint32_t)length;
        ecat_foe_crc32_update(&t->running_crc32, data, length);
    } else {
        /* DATA 普通数据流导入 */
        if ((uint32_t)length > (UINT32_MAX - t->current_offset) ||
            t->current_offset + (uint32_t)length > t->target->max_size) {
            t->last_error = ECAT_TRANSFER_ERR_BOUNDARY_EXCEEDED;
            if (t->driver_initialized && t->target->driver.io.abort != NULL) {
                t->target->driver.io.abort(t->target->user_ctx);
            }
            t->driver_initialized = false;
            t->active = false;
            release_session(t, owner, token);
            return ECAT_TRANSFER_FAILED;
        }

        /* 按需擦除 */
        uint32_t need_end = t->current_offset + (uint32_t)length;
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
            t->current_offset, data, (uint32_t)length, t->target->user_ctx);
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

        t->current_offset += (uint32_t)length;
        ecat_foe_crc32_update(&t->running_crc32, data, length);
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
            /* DATA 模式 */
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

    if (t->core_started && t->core != NULL) {
        ecat_foe_core_on_abort(t->core);
    } else if (t->driver_initialized && t->target != NULL) {
        if (t->direction == ECAT_TRANSFER_DIR_EXPORT && t->target->driver.read_close != NULL) {
            t->target->driver.read_close(t->target->user_ctx);
        } else if (t->target->driver.io.abort != NULL) {
            t->target->driver.io.abort(t->target->user_ctx);
        }
    }

    t->core_started = false;
    t->driver_initialized = false;
    t->active = false;
    release_session(t, owner, token);
    return ECAT_TRANSFER_COMPLETE;
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
