/**
 * @file ecat_foe_ssc.c
 * @brief Beckhoff SSC (Slave Stack Code) 协议栈适配层实现
 *
 * 重要声明与边界限制：
 * 本模块为通用固件升级核心状态机接入 Beckhoff Slave Stack Code (SSC) 的薄转接层设计。
 * 本机当前缺乏真实 Beckhoff SSC 官方源码包（受 ETG 许可约束需官方会员账号导出），
 * 因此本模块尚未在真实 SSC 协议栈与物理硬件上进行联动测试。
 * 严禁将本模块的抽象契约测试宣称为真实 SSC 验证！
 */

#include "ecat_foe_ssc.h"
#include <string.h>

void ecat_foe_ssc_init(ecat_foe_ssc_adapter_t *adapter, ecat_foe_core_t *core, uint32_t max_packet_size)
{
    if (adapter == NULL) {
        return;
    }
    adapter->core = core;
    memset(&adapter->transport, 0, sizeof(adapter->transport));
    adapter->async_gate = NULL;
    adapter->max_packet_size = max_packet_size;
    adapter->expected_password = 0U;
    adapter->session_active = false;
    adapter->async_final_acked = false;
}

void ecat_foe_ssc_init_async(ecat_foe_ssc_adapter_t *adapter,
                             ecat_foe_core_t *core,
                             ecat_foe_ssc_async_t *async_gate)
{
    if (adapter == NULL) {
        return;
    }
    adapter->core = core;
    memset(&adapter->transport, 0, sizeof(adapter->transport));
    adapter->async_gate = async_gate;
    adapter->max_packet_size = 0U;
    adapter->expected_password = 0U;
    adapter->session_active = false;
    adapter->async_final_acked = false;
}

int ecat_foe_ssc_bind_upgrade(ecat_foe_ssc_adapter_t *a, ecat_upgrade_t *upgrade)
{
    if (a == NULL || upgrade == NULL || upgrade->core == NULL || a->core != upgrade->core ||
        a->transport.upgrade != NULL || a->session_active) return -1;
    if (a->async_gate != NULL && (a->async_gate->pending || a->async_gate->completed ||
        a->async_gate->ops != ecat_foe_transport_async_ops() ||
        a->async_gate->ops_ctx != &a->transport)) return -1;
    if (a->async_gate == NULL) {
        for (size_t i = 0; i < ECAT_FOE_TARGET_COUNT; i++)
            if (upgrade->core->cfg.targets[i].enabled && upgrade->core->cfg.targets[i].asynchronous)
                return -1;
    }
    return ecat_foe_transport_init(&a->transport, upgrade);
}

static void ssc_abort_core(ecat_foe_ssc_adapter_t *a)
{
    if (a->transport.upgrade != NULL) ecat_foe_transport_abort(&a->transport);
    else ecat_foe_core_on_abort(a->core);
}

static int ssc_end_core(ecat_foe_ssc_adapter_t *a)
{
    if (a->transport.upgrade == NULL) return ecat_foe_core_on_end(a->core);
    int rc = ecat_foe_transport_start(&a->transport, NULL, 0U, false) == ECAT_UPGRADE_ASYNC_COMPLETE ? 0 : -1;
    if (rc == 0) a->async_final_acked = true;
    return rc;
}

static bool ssc_completed(const ecat_foe_ssc_adapter_t *a)
{
    return a->transport.upgrade != NULL ? a->async_final_acked :
        (a->core->state == ECAT_FOE_STATE_SUCCESS || a->async_final_acked);
}

int ecat_foe_ssc_set_expected_password(ecat_foe_ssc_adapter_t *adapter,
                                      uint32_t expected_password)
{
    if (adapter == NULL || adapter->session_active ||
        (adapter->async_gate != NULL &&
         (adapter->async_gate->pending || adapter->async_gate->completed))) {
        return -1;
    }
    adapter->expected_password = expected_password;
    return 0;
}

uint16_t ecat_foe_ssc_on_write(ecat_foe_ssc_adapter_t *adapter, const char *pName, uint16_t nameSize, uint32_t password)
{
    if (adapter == NULL || adapter->core == NULL || pName == NULL) {
        return ECAT_FOE_SSC_ERR_ILLEGAL;
    }
    if (password != adapter->expected_password) {
        return ECAT_FOE_SSC_ERR_NORIGHTS_PW;
    }
    adapter->async_final_acked = false;

    if (adapter->async_gate != NULL && adapter->session_active) {
        ecat_foe_ssc_async_abort_session(adapter->async_gate);
        ssc_abort_core(adapter);
        adapter->session_active = false;
    } else if (adapter->async_gate != NULL &&
               (adapter->async_gate->pending || adapter->async_gate->completed)) {
        ecat_foe_ssc_async_abort(adapter->async_gate);
    }
    /* 调用通用核心机处理 WRQ 请求，核验项目名称、硬件目标与防回滚版本 */
    int rc = adapter->transport.upgrade != NULL
        ? ecat_foe_transport_begin(&adapter->transport, pName, (size_t)nameSize)
        : ecat_foe_core_on_wrq(adapter->core, pName, (size_t)nameSize);
    if (rc != 0) {
        adapter->session_active = false;
        /* 保留核心机计算的标准 FoE 错误码 (如 0x8001 NOTFOUND, 0x8004 ILLEGAL 等) */
        return (uint16_t)rc;
    }

    adapter->session_active = true;
    return 0U; /* 0 表示成功 */
}

uint16_t ecat_foe_ssc_on_data(ecat_foe_ssc_adapter_t *adapter, const uint8_t *pData, uint16_t length)
{
    if (adapter == NULL || adapter->core == NULL) {
        return ECAT_FOE_SSC_ERR_PROGERROR;
    }
    if (adapter->async_gate != NULL) {
        return ECAT_FOE_SSC_ERR_ILLEGAL;
    }

    /* 0 长度包处理：
     * 1. 若会话已通过短包成功结束 (core 处于 SUCCESS 态)，主站补发的 0 长度确认包作为幂等握手直接返回成功；
     * 2. 若会话处于激活状态，触发 0 长度包显式 EOF 定界；
     * 3. 其余非激活状态拒绝非法操作。
     */
    if (length == 0U) {
        if (!adapter->session_active) {
            if (ssc_completed(adapter)) {
                return 0U; /* 短包结束后的幂等 0 长度确认包 */
            }
            return ECAT_FOE_SSC_ERR_ILLEGAL;
        }

        if (ssc_end_core(adapter) != 0) {
            adapter->session_active = false;
            return ECAT_FOE_SSC_ERR_PROGERROR;
        }
        adapter->session_active = false;
        return 0U;
    }

    /* 非零长度包必须具备激活会话且数据指针非空 */
    if (!adapter->session_active) {
        return ECAT_FOE_SSC_ERR_ILLEGAL;
    }

    if (pData == NULL) {
        adapter->session_active = false;
        ssc_abort_core(adapter);
        return ECAT_FOE_SSC_ERR_ILLEGAL;
    }

    /* 包长上限校验：单包大小不得超出适配层声明的最大邮箱容量 */
    if (adapter->max_packet_size > 0U && (uint32_t)length > adapter->max_packet_size) {
        adapter->session_active = false;
        ssc_abort_core(adapter);
        return ECAT_FOE_SSC_ERR_ILLEGAL;
    }

    /* 写入常规数据块 */
    int data_rc = adapter->transport.upgrade != NULL
        ? (ecat_foe_transport_start(&adapter->transport, pData, length, true) == ECAT_UPGRADE_ASYNC_COMPLETE ? 0 : -1)
        : ecat_foe_core_on_data(adapter->core, pData, (size_t)length);
    if (data_rc != 0) {
        adapter->session_active = false;
        return ECAT_FOE_SSC_ERR_PROGERROR;
    }

    /* 依据短包定界传输结束：若当前块长度小于单包最大容量，判定为最后一包 (EOF) */
    if (adapter->max_packet_size > 0U && (uint32_t)length < adapter->max_packet_size) {
        if (ssc_end_core(adapter) != 0) {
            adapter->session_active = false;
            return ECAT_FOE_SSC_ERR_PROGERROR;
        }
        adapter->session_active = false;
    }

    return 0U;
}

uint16_t ecat_foe_ssc_on_data_following(ecat_foe_ssc_adapter_t *adapter,
                                        const uint8_t *pData,
                                        uint16_t length,
                                        bool data_following,
                                        uint32_t now_ticks)
{
    if (adapter == NULL || adapter->core == NULL || adapter->async_gate == NULL) {
        return ECAT_FOE_SSC_ERR_ILLEGAL;
    }
    if (!adapter->session_active) {
        if (adapter->async_final_acked && length == 0U && !data_following) {
            return 0U;
        }
        return ECAT_FOE_SSC_ERR_ILLEGAL;
    }

    uint16_t result = ecat_foe_ssc_async_on_data(adapter->async_gate,
                                                 pData,
                                                 (size_t)length,
                                                 data_following,
                                                 now_ticks);
    if (adapter->async_gate->pending && result == adapter->async_gate->busy_foe_code) {
        return result;
    }
    if (result != 0U) {
        adapter->session_active = false;
        adapter->async_final_acked = false;
        ecat_foe_ssc_async_abort(adapter->async_gate);
        if (adapter->core != NULL) {
            ssc_abort_core(adapter);
        }
        return result;
    }
    if (!data_following) {
        if (adapter->transport.upgrade != NULL ? !adapter->transport.committed :
            adapter->core->state != ECAT_FOE_STATE_SUCCESS) {
            ecat_foe_ssc_async_abort(adapter->async_gate);
            ssc_abort_core(adapter);
            adapter->session_active = false;
            adapter->async_final_acked = false;
            return ECAT_FOE_SSC_ERR_PROGERROR;
        }
        adapter->session_active = false;
        adapter->async_final_acked = true;
    }
    return 0U;
}

uint16_t ecat_foe_ssc_on_main_loop(ecat_foe_ssc_adapter_t *adapter,
                                   uint32_t now_ticks)
{
    if (adapter == NULL || adapter->async_gate == NULL) {
        return ECAT_FOE_SSC_ERR_ILLEGAL;
    }
    uint16_t result = ecat_foe_ssc_async_poll(adapter->async_gate, now_ticks);
    if (result == adapter->async_gate->operation_error_foe_code &&
        adapter->async_gate->completed && adapter->async_gate->operation_failed &&
        adapter->core != NULL) {
        /* Timeout and driver failures happen in the main loop, outside DATA
         * callback routing. Close verifier/core state here; preserve the gate's
         * completed packet so SSC's retransmission receives the configured error. */
        ssc_abort_core(adapter);
        adapter->session_active = true;
        adapter->async_final_acked = false;
    }
    return result;
}

uint16_t ecat_foe_ssc_on_end(ecat_foe_ssc_adapter_t *adapter)
{
    if (adapter == NULL || adapter->core == NULL) {
        return ECAT_FOE_SSC_ERR_PROGERROR;
    }

    if (adapter->async_gate != NULL) {
        if (!adapter->session_active &&
            ssc_completed(adapter)) {
            return 0U;
        }
        return adapter->async_gate->pending
            ? adapter->async_gate->busy_foe_code : ECAT_FOE_SSC_ERR_ILLEGAL;
    }

    /* 若会话已通过短包正常结束并达成 SUCCESS，返回成功 */
    if (!adapter->session_active) {
        return ssc_completed(adapter) ? 0U : ECAT_FOE_SSC_ERR_ILLEGAL;
    }

    if (ssc_end_core(adapter) != 0) {
        adapter->session_active = false;
        return ECAT_FOE_SSC_ERR_PROGERROR;
    }

    adapter->session_active = false;
    return 0U;
}

void ecat_foe_ssc_on_abort(ecat_foe_ssc_adapter_t *adapter, uint16_t errorCode)
{
    (void)errorCode;
    if (adapter == NULL || adapter->core == NULL) {
        return;
    }

    /* 若固件升级已顺利成功落盘 (SUCCESS)，保留终态标记，不予重置为 IDLE */
    if (!adapter->session_active &&
        ssc_completed(adapter)) {
        return;
    }

    if (adapter->async_gate != NULL &&
        (adapter->session_active || adapter->async_gate->pending ||
         adapter->async_gate->completed)) {
        ecat_foe_ssc_async_abort_session(adapter->async_gate);
    }
    ssc_abort_core(adapter);
    adapter->session_active = false;
    adapter->async_final_acked = false;
}
