/**
 * @file ecat_foe_soes.c
 * @brief SOES 协议栈适配层实现
 *
 * 严格遵循 SAD-v0.1 第 1.2 节规范：
 * - 纯 C99 实现，零动态内存
 * - 仅依靠标准 SOES 接口，不改动任何 SOES 源码
 * - 写入回调内纯同步阻塞
 * - 以 length < buffer_size 作为结束判定依据 (包含 length == 0)
 * - 状态机 BOOT->INIT 迁移拦截 (post_state_change_hook)
 */

#include "ecat_foe_soes.h"
#include <string.h>

static ecat_foe_soes_adapter_t *s_active_adapter = NULL;

void ecat_foe_soes_init(ecat_foe_soes_adapter_t *adapter, ecat_foe_core_t *core, uint32_t buffer_size)
{
    if (adapter == NULL) {
        return;
    }
    adapter->core = core;
    adapter->buffer_size = buffer_size;
    adapter->last_commit_failed = false;
    adapter->last_al_error = 0;
    s_active_adapter = adapter;
}

int ecat_foe_soes_on_wrq(ecat_foe_soes_adapter_t *adapter, const char *filename, size_t filename_len)
{
    if (adapter == NULL || adapter->core == NULL || filename == NULL) {
        return ECAT_FOE_ERRCODE_NOTDEFINED;
    }
    adapter->last_commit_failed = false;
    adapter->last_al_error = 0;
    s_active_adapter = adapter;
    return ecat_foe_core_on_wrq(adapter->core, filename, filename_len);
}

uint32_t ecat_foe_soes_write_cb(foe_file_cfg_t *self, uint8_t *data, size_t length)
{
    (void)self;
    ecat_foe_soes_adapter_t *adapter = s_active_adapter;
    if (adapter == NULL || adapter->core == NULL) {
        return 1U; /* 非零表示失败 */
    }

    /* 0 长度包结束路径 */
    if (length == 0) {
        if (ecat_foe_core_on_end(adapter->core) != 0) {
            adapter->last_commit_failed = true;
            return 1U;
        }
        return 0U;
    }

    /* 写入数据块 */
    if (ecat_foe_core_on_data(adapter->core, data, length) != 0) {
        adapter->last_commit_failed = true;
        return 1U;
    }

    /* 回调级结束判定: 当本次接收字节数小于 buffer_size 时判定为最后一包 (短包路径) */
    if (adapter->buffer_size > 0 && length < adapter->buffer_size) {
        if (ecat_foe_core_on_end(adapter->core) != 0) {
            adapter->last_commit_failed = true;
            return 1U;
        }
    }

    return 0U; /* 0 表示成功 */
}

void ecat_foe_soes_post_state_hook(ecat_foe_soes_adapter_t *adapter, uint8_t *as, uint8_t *an)
{
    if (adapter == NULL || as == NULL || an == NULL) {
        return;
    }

    /* 依据 SAD 1.2 缓解措施 2: 检测 BOOT -> INIT 迁移 (0x13)
     * 时序说明：
     * 1. 当该 post hook 触发时，SOES esc.c:1193 已执行 ESC_stopmbx()，此时物理邮箱已被停用；
     * 2. 若在此处改写 *an 为 BOOT|Error 并记录 AL 错误码 0x0014 (ALERR_NOVALIDFIRMWARE)，
     *    可阻断从站进入 INIT 正常通信态；
     * 3. 局限性说明：当前为适配桩单测验证，真实硬件环境下由于邮箱已停止，主站无法在该状态下通过 FoE 继续交互，
     *    必须先按 EtherCAT 状态机规范执行故障复位后重新启动升级流程。
     */
    if (*as == 0x13U && adapter->last_commit_failed) {
        /* 改写目标状态为 BOOT + Error (0x03 | 0x10 = 0x13) */
        *an = 0x03U | 0x10U;
        adapter->last_al_error = 0x0014U; /* ALERR_NOVALIDFIRMWARE (0x0014) */
    }
}
