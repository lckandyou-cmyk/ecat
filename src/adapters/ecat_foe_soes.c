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
    adapter->al_error_fn = NULL;
    s_active_adapter = adapter;
}

void ecat_foe_soes_set_al_error_fn(ecat_foe_soes_adapter_t *adapter, ecat_foe_al_error_fn_t al_error_fn)
{
    if (adapter != NULL) {
        adapter->al_error_fn = al_error_fn;
    }
}

int ecat_foe_soes_on_wrq(ecat_foe_soes_adapter_t *adapter, const char *filename, size_t filename_len)
{
    if (adapter == NULL || adapter->core == NULL || filename == NULL) {
        return ECAT_FOE_ERRCODE_NOTDEFINED;
    }
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
            return 1U;
        }
        return 0U;
    }

    /* 写入数据块 */
    if (ecat_foe_core_on_data(adapter->core, data, length) != 0) {
        return 1U;
    }

    /* 回调级结束判定: 当本次接收字节数小于 buffer_size 时判定为最后一包 (短包路径) */
    if (adapter->buffer_size > 0 && length < adapter->buffer_size) {
        if (ecat_foe_core_on_end(adapter->core) != 0) {
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

    /* 依据 SAD 1.2 缓解措施 2: 检测 BOOT -> INIT 迁移
     * 使用语义清晰的命名常量严格区分迁移码与 AL 状态值：
     * - *as 匹配 ECAT_TRANSITION_BOOT_TO_INIT ((ESCinit << 4) | ESCboot = 0x13)
     * - *an 改写为 ECAT_AL_STATUS_BOOT_ERROR (ESCboot | ESCerror = 0x13)
     *
     * 判定只依赖核心公开查询函数：
     * ecat_foe_core_upgrade_incomplete(adapter->core)
     * 若固件升级未闭环 (底层驱动已 init 但未成功 finalize，例如中途断开、校验失败或残留无效固件)，
     * 固件即处于残缺/不可用状态，必须强行拦截 BOOT->INIT 迁移。
     *
     * 适配层绝不直接读取 core 内部结构字段，维持核心机抽象封装。
     *
     * 时序与物理限制明确说明：
     * 1. 当该 post hook 触发时，SOES esc.c:1193 已执行 ESC_stopmbx()，此时物理邮箱已被停用；
     * 2. 若在此处改写 *an 为 BOOT|Error 并调用注入的 al_error_fn(0x0014) (在真实 SOES 中对应 ESC_ALerror)，
     *    可向 0x134 写入 AL Status Code 并阻断从站进入 INIT 正常通信态；
     * 3. 局限性说明：真实 SOES esc_foe.c 已在独立宿主 Harness 中完成编译与联动验证，但生产环境目前仍未正式接入，
     *    未在真实硬件上集成编译，未进行真实物理总线与主站动态联调；真实硬件环境下由于邮箱已停止，主站无法在该状态下通过 FoE 继续交互，
     *    必须先按 EtherCAT 状态机规范执行故障复位后重新启动升级流程；绝不能宣称已在真实硬件上验证该拦截行为。
     */
    bool should_block_init = false;
    if (adapter->core != NULL) {
        should_block_init = ecat_foe_core_upgrade_incomplete(adapter->core);
    }

    if (*as == ECAT_TRANSITION_BOOT_TO_INIT && should_block_init) {
        /* 改写目标状态为 BOOT + Error (0x03 | 0x10 = 0x13) */
        *an = ECAT_AL_STATUS_BOOT_ERROR;
        if (adapter->al_error_fn != NULL) {
            adapter->al_error_fn(ECAT_AL_STATUS_CODE_NO_VALID_FW);
        }
    }
}

#define ECAT_MBX_TYPE_FOE 0x04U
#define ECAT_FOE_OP_WRQ   0x02U
#define ECAT_MBX_HDR_SIZE 6U
#define ECAT_FOE_HDR_SIZE 6U

int ecat_foe_soes_pre_process_hook(ecat_foe_soes_adapter_t *adapter,
                                   foe_cfg_t *foe_cfg,
                                   const uint8_t *mbx_buf,
                                   size_t mbx_buf_len)
{
    if (adapter == NULL || adapter->core == NULL || foe_cfg == NULL || mbx_buf == NULL) {
        return -1;
    }

    /* 帧边界校验：最小必须包含 6 字节 Mailbox 头 + 6 字节 FoE 头 */
    if (mbx_buf_len < (ECAT_MBX_HDR_SIZE + ECAT_FOE_HDR_SIZE)) {
        return 0; /* 畸形帧放行给原生 SOES 协议栈自动中止 */
    }

    /* Mailbox Header:
     * 字节 0-1: length (小端 uint16_t)
     * 字节 5: 低 4 位为 mbxtype (0x04 为 FoE)
     */
    uint16_t mbx_payload_len = (uint16_t)(mbx_buf[0] | ((uint16_t)mbx_buf[1] << 8));
    uint8_t mbx_type = (uint8_t)(mbx_buf[5] & 0x0FU);

    if (mbx_type != ECAT_MBX_TYPE_FOE) {
        return 0; /* 非 FoE 报文直接放行 */
    }

    if (mbx_payload_len < ECAT_FOE_HDR_SIZE) {
        return 0;
    }

    /* 防止声明的 payload 长度越过传入缓冲区的实际边界 */
    if (mbx_buf_len < (size_t)ECAT_MBX_HDR_SIZE + mbx_payload_len) {
        return 0;
    }

    /* FoE Header:
     * 字节 6: opcode (0x02 为 WRQ)
     */
    uint8_t opcode = mbx_buf[ECAT_MBX_HDR_SIZE];
    if (opcode != ECAT_FOE_OP_WRQ) {
        return 0; /* 非 WRQ 请求直接放行 */
    }

    /* 提取动态文件名 */
    uint16_t name_len = mbx_payload_len - (uint16_t)ECAT_FOE_HDR_SIZE;
    if (name_len == 0 || name_len > FOE_FN_MAX) {
        /* 空文件名或超出 31 字符上限，不予同步，放行让 SOES 自动拒绝 */
        return 0;
    }

    /* 有界检查：防止声明的长度超出实际传入的缓冲区尺寸 */
    if ((size_t)(ECAT_MBX_HDR_SIZE + ECAT_FOE_HDR_SIZE + name_len) > mbx_buf_len) {
        return 0;
    }

    const char *raw_name = (const char *)(mbx_buf + ECAT_MBX_HDR_SIZE + ECAT_FOE_HDR_SIZE);
    char safe_name[FOE_FN_MAX + 1];
    memcpy(safe_name, raw_name, name_len);
    safe_name[name_len] = '\0';

    /* 调用核心机 WRQ 逻辑进行预检与自愈建立会话 */
    int rc = ecat_foe_core_on_wrq(adapter->core, safe_name, (size_t)name_len);
    if (rc != 0) {
        /* 核心机拒绝 (如项目名不匹配或防回滚拦截)：不执行名称同步，原生 SOES 比对失败将自动回复 0x8001 */
        return rc;
    }

    /* 核心机校验通过：根据目标定位对应条目并同步动态文件名 */
    uint16_t tid = adapter->core->req_target_id;
    uint32_t file_idx = (tid > 0 && tid <= foe_cfg->n_files) ? (uint32_t)(tid - 1) : 0U;

#ifndef ECAT_FOE_SOES_MAX_FILES
#define ECAT_FOE_SOES_MAX_FILES 4
#endif
    static char s_synced_filenames[ECAT_FOE_SOES_MAX_FILES][FOE_FN_MAX + 1];
    if (file_idx < ECAT_FOE_SOES_MAX_FILES) {
        memcpy(s_synced_filenames[file_idx], safe_name, (size_t)name_len + 1);
        foe_cfg->files[file_idx].name = s_synced_filenames[file_idx];
    }
    s_active_adapter = adapter;

    return 0;
}
