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
    adapter->has_staged_wrq = false;
    adapter->staged_filename[0] = '\0';
    adapter->staged_target_id = 0;
    adapter->original_n_files = 0;
    memset(&adapter->mbx_ctx, 0, sizeof(adapter->mbx_ctx));
    adapter->mbx_ctx.newly_claimed_slot = -1;
    adapter->target_to_file_map[0] = -1;
    for (size_t i = 1; i <= ECAT_FOE_TARGET_COUNT; i++) {
        adapter->target_to_file_map[i] = (int8_t)(i - 1);
    }
    s_active_adapter = adapter;
}

void ecat_foe_soes_reset_target_file_map(ecat_foe_soes_adapter_t *adapter)
{
    if (adapter == NULL) {
        return;
    }
    for (size_t i = 0; i <= ECAT_FOE_TARGET_COUNT; i++) {
        adapter->target_to_file_map[i] = -1;
    }
}

int ecat_foe_soes_set_target_file_map(ecat_foe_soes_adapter_t *adapter, uint16_t target_id, int32_t file_idx)
{
    if (adapter == NULL) {
        return -1;
    }
    if (target_id < 1 || target_id > ECAT_FOE_TARGET_COUNT) {
        return -1;
    }
    if (file_idx < 0) {
        adapter->target_to_file_map[target_id] = -1;
        return 0;
    }
    if ((uint32_t)file_idx >= ECAT_FOE_SOES_MAX_FILES) {
        return -1;
    }
    /* 防御重复目标映射：拒绝多个不同目标映射至相同文件表索引，防止名称与配置冲突 */
    for (size_t t = 1; t <= ECAT_FOE_TARGET_COUNT; t++) {
        if (t != target_id && adapter->target_to_file_map[t] == (int8_t)file_idx) {
            return -1;
        }
    }
    adapter->target_to_file_map[target_id] = (int8_t)file_idx;
    return 0;
}

int ecat_foe_soes_set_mbx_context(ecat_foe_soes_adapter_t *adapter,
                                  const void *mbx_control,
                                  uint8_t *mbx_buffers,
                                  uint8_t num_buffers,
                                  uint16_t buffer_size)
{
    if (adapter == NULL || mbx_control == NULL || mbx_buffers == NULL ||
        num_buffers < 2 || num_buffers > ECAT_FOE_SOES_MAX_MBX_BUFFERS ||
        buffer_size < (ECAT_MBX_HDR_SIZE + ECAT_FOE_HDR_SIZE)) {
        return -1;
    }
    adapter->mbx_ctx.mbx_control_states = (const uint8_t *)mbx_control;
    adapter->mbx_ctx.mbx_buffers = mbx_buffers;
    adapter->mbx_ctx.num_buffers = num_buffers;
    adapter->mbx_ctx.buffer_size = buffer_size;
    adapter->mbx_ctx.newly_claimed_slot = -1;
    memset(adapter->mbx_ctx.pre_states, 0, sizeof(adapter->mbx_ctx.pre_states));
    return 0;
}

int8_t ecat_foe_soes_get_newly_claimed_slot(const ecat_foe_soes_adapter_t *adapter)
{
    if (adapter == NULL) {
        return -1;
    }
    return adapter->mbx_ctx.newly_claimed_slot;
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

    /* 安全防御：若存在尚未被后置钩子确认的暂存 WRQ，说明时序错乱或后置校验被绕过，拒绝写入并释放暂存 */
    if (adapter->has_staged_wrq) {
        adapter->has_staged_wrq = false;
        adapter->staged_filename[0] = '\0';
        adapter->staged_target_id = 0;
        return 1U;
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
#define ECAT_FOE_OP_ACK   0x04U
#define ECAT_FOE_OP_ERR   0x05U
#define ECAT_MBX_HDR_SIZE 6U
#define ECAT_FOE_HDR_SIZE 6U
#define ECAT_FOE_ERR_NOTDEFINED 0x8000U

static void foe_convert_slot_to_error(uint8_t *slot_buf, uint16_t buffer_size, uint32_t error_code)
{
    if (slot_buf == NULL || buffer_size < (ECAT_MBX_HDR_SIZE + ECAT_FOE_HDR_SIZE)) {
        return;
    }
    slot_buf[0] = (uint8_t)(ECAT_FOE_HDR_SIZE & 0xFF);
    slot_buf[1] = (uint8_t)((ECAT_FOE_HDR_SIZE >> 8) & 0xFF);
    slot_buf[2] = 0x00;
    slot_buf[3] = 0x00;
    slot_buf[4] = 0x00;
    slot_buf[5] = ECAT_MBX_TYPE_FOE;
    slot_buf[ECAT_MBX_HDR_SIZE + 0] = ECAT_FOE_OP_ERR;
    slot_buf[ECAT_MBX_HDR_SIZE + 1] = 0x00;
    slot_buf[ECAT_MBX_HDR_SIZE + 2] = (uint8_t)(error_code & 0xFF);
    slot_buf[ECAT_MBX_HDR_SIZE + 3] = (uint8_t)((error_code >> 8) & 0xFF);
    slot_buf[ECAT_MBX_HDR_SIZE + 4] = (uint8_t)((error_code >> 16) & 0xFF);
    slot_buf[ECAT_MBX_HDR_SIZE + 5] = (uint8_t)((error_code >> 24) & 0xFF);
}

static void foe_neutralize_slot_if_ack(uint8_t *slot_buf, uint16_t buffer_size)
{
    if (slot_buf == NULL || buffer_size < (ECAT_MBX_HDR_SIZE + ECAT_FOE_HDR_SIZE)) {
        return;
    }
    uint8_t mbx_type = (uint8_t)(slot_buf[5] & 0x0FU);
    uint8_t opcode = slot_buf[ECAT_MBX_HDR_SIZE];
    /* 若该槽位包含待发的 FoE ACK(0)，改写为 FOE_OP_ERR (0x8000)，提供主站可观察的一致失败 */
    if (mbx_type == ECAT_MBX_TYPE_FOE && opcode == ECAT_FOE_OP_ACK) {
        foe_convert_slot_to_error(slot_buf, buffer_size, ECAT_FOE_ERR_NOTDEFINED);
    }
}

#ifndef ECAT_FOE_SOES_MAX_FILES
#define ECAT_FOE_SOES_MAX_FILES 4
#endif
static char s_synced_filenames[ECAT_FOE_SOES_MAX_FILES][FOE_FN_MAX + 1] = {{0}};

int ecat_foe_soes_pre_process_hook(ecat_foe_soes_adapter_t *adapter,
                                   foe_cfg_t *foe_cfg,
                                   const uint8_t *mbx_buf,
                                   size_t mbx_buf_len)
{
    if (adapter == NULL || adapter->core == NULL || foe_cfg == NULL || mbx_buf == NULL) {
        return -1;
    }

    /* 必须配置邮箱上下文以保证新鲜度与状态关联；未配置时无法确保响应关联，严格拒绝 */
    if (adapter->mbx_ctx.mbx_control_states == NULL ||
        adapter->mbx_ctx.mbx_buffers == NULL ||
        adapter->mbx_ctx.num_buffers < 2) {
        return -1;
    }

    /* 对所有输出邮箱槽位状态进行只读快照 (不向 SOES 写入任何数据，绝不篡改 SOES 内存) */
    adapter->mbx_ctx.newly_claimed_slot = -1;
    for (uint8_t i = 1; i < adapter->mbx_ctx.num_buffers; i++) {
        adapter->mbx_ctx.pre_states[i] = adapter->mbx_ctx.mbx_control_states[i];
    }

    /* 记录并维护从站初始真实条目数 */
    if (adapter->original_n_files == 0 && foe_cfg->n_files > 0) {
        adapter->original_n_files = foe_cfg->n_files;
    }
    /* 每次前置钩子启动时，恢复真实条目数 */
    if (adapter->original_n_files > 0) {
        foe_cfg->n_files = adapter->original_n_files;
    }

    /* 帧边界校验：最小必须包含 6 字节 Mailbox 头 + 6 字节 FoE 头 */
    if (mbx_buf_len < (ECAT_MBX_HDR_SIZE + ECAT_FOE_HDR_SIZE)) {
        return 0; /* 畸形短帧放行给原生 SOES 协议栈自动中止 */
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
        /* 空文件名或超出 31 字符上限：
         * 将 foe_cfg->n_files 置 0，使原生 SOES 在 FOE_fopen 循环 0 次直接返回 0x8001，
         * 坚决杜绝原生 strncmp 长度为 0 时误匹配 files[0]！
         */
        foe_cfg->n_files = 0;
        adapter->has_staged_wrq = false;
        return -1;
    }

    /* 有界检查：防止声明的长度超出实际传入的缓冲区尺寸 */
    if ((size_t)(ECAT_MBX_HDR_SIZE + ECAT_FOE_HDR_SIZE + name_len) > mbx_buf_len) {
        foe_cfg->n_files = 0;
        adapter->has_staged_wrq = false;
        return -1;
    }

    const char *raw_name = (const char *)(mbx_buf + ECAT_MBX_HDR_SIZE + ECAT_FOE_HDR_SIZE);
    char safe_name[FOE_FN_MAX + 1];
    memcpy(safe_name, raw_name, name_len);
    safe_name[name_len] = '\0';

    /* 调用核心机只读预检函数校验文件名、目标与版本合法性 (不产生状态机变更或驱动 abort 副作用) */
    uint16_t tid = 0;
    uint32_t ver = 0;
    int vrc = ecat_foe_core_validate_wrq(adapter->core, safe_name, (size_t)name_len, &tid, &ver);
    if (vrc != 0) {
        /* 核心机校验失败 (项目不符、短前缀、非法格式等)：
         * 关键安全防御：将 foe_cfg->n_files 置 0，切断原生 SOES 命中旧缓存或短前缀的路径！
         * 原生 SOES 将直接回复 FOE_ERR_NOTFOUND (0x8001)；
         * 绝不调用 core_on_wrq，已有活跃会话完好无损。
         */
        foe_cfg->n_files = 0;
        adapter->has_staged_wrq = false;
        return vrc;
    }

    /* 严格目标范围校验：通过目标映射表查找文件条目索引，支持稀疏与紧凑文件表 */
    int8_t mapped_idx = (tid <= ECAT_FOE_TARGET_COUNT) ? adapter->target_to_file_map[tid] : -1;
    if (mapped_idx < 0 || (uint32_t)mapped_idx >= foe_cfg->n_files || (uint32_t)mapped_idx >= ECAT_FOE_SOES_MAX_FILES) {
        foe_cfg->n_files = 0;
        adapter->has_staged_wrq = false;
        return -1;
    }

    uint32_t file_idx = (uint32_t)mapped_idx;

    /* 同步动态文件名至对应条目独立的专属缓冲区 */
    memcpy(s_synced_filenames[file_idx], safe_name, (size_t)name_len + 1);
    foe_cfg->files[file_idx].name = s_synced_filenames[file_idx];

    /* 暂存该合规 WRQ，等待原生 SOES 校验密码与 BOOT 状态接受后再行激活会话 */
    adapter->has_staged_wrq = true;
    memcpy(adapter->staged_filename, safe_name, (size_t)name_len + 1);
    adapter->staged_target_id = tid;
    s_active_adapter = adapter;

    return 0;
}

int ecat_foe_soes_post_process_hook(ecat_foe_soes_adapter_t *adapter,
                                    foe_cfg_t *foe_cfg,
                                    const uint8_t *mbx_out_buf,
                                    size_t mbx_out_len)
{
    if (adapter == NULL || foe_cfg == NULL) {
        return -1;
    }

    /* 恢复真实条目数 */
    if (adapter->original_n_files > 0) {
        foe_cfg->n_files = adapter->original_n_files;
    }

    if (!adapter->has_staged_wrq) {
        return 0;
    }

    /* 若未配置邮箱上下文，无法保证新鲜度与关联，严格失败关闭 */
    if (adapter->mbx_ctx.mbx_control_states == NULL ||
        adapter->mbx_ctx.mbx_buffers == NULL ||
        adapter->mbx_ctx.num_buffers < 2) {
        adapter->has_staged_wrq = false;
        adapter->staged_filename[0] = '\0';
        adapter->staged_target_id = 0;
        return -1;
    }

    /* 扫描输出邮箱槽位状态迁移：寻找在本轮处理中由 MBXstate_idle 跃迁至 MBXstate_outreq 的新认领槽位 */
    int newly_claimed = -1;
    int count_new = 0;
    for (uint8_t i = 1; i < adapter->mbx_ctx.num_buffers; i++) {
        uint8_t pre = adapter->mbx_ctx.pre_states[i];
        uint8_t cur = adapter->mbx_ctx.mbx_control_states[i];
        if (pre == MBXstate_idle && cur == MBXstate_outreq) {
            newly_claimed = (int)i;
            count_new++;
        }
    }

    /* Case A: 若无新槽位认领 (如输出邮箱耗尽、无可用缓冲) */
    if (count_new == 0) {
        adapter->has_staged_wrq = false;
        adapter->staged_filename[0] = '\0';
        adapter->staged_target_id = 0;
        return 0; /* 无待发报文产生，安全释放暂存 */
    }

    /* Case B: 检测到多个新槽位同时跃迁为 outreq (违反单执行流无插入互斥契约) */
    if (count_new > 1) {
        /* 扫描所有新跃迁槽位，若包含待发的 ACK 则中和为 FOE_OP_ERR，杜绝主站收到假 ACK */
        for (uint8_t i = 1; i < adapter->mbx_ctx.num_buffers; i++) {
            uint8_t pre = adapter->mbx_ctx.pre_states[i];
            uint8_t cur = adapter->mbx_ctx.mbx_control_states[i];
            if (pre == MBXstate_idle && cur == MBXstate_outreq) {
                uint8_t *slot_ptr = &adapter->mbx_ctx.mbx_buffers[i * adapter->mbx_ctx.buffer_size];
                foe_neutralize_slot_if_ack(slot_ptr, adapter->mbx_ctx.buffer_size);
            }
        }
        adapter->has_staged_wrq = false;
        adapter->staged_filename[0] = '\0';
        adapter->staged_target_id = 0;
        return -1; /* 契约破坏，安全中和并报错 */
    }

    /* Case C: 唯一匹配到新认领槽位 newly_claimed */
    adapter->mbx_ctx.newly_claimed_slot = (int8_t)newly_claimed;
    uint8_t *slot_buf = &adapter->mbx_ctx.mbx_buffers[newly_claimed * adapter->mbx_ctx.buffer_size];

    /* 若外部调用显式传入了 mbx_out_buf，则必须与实际新认领的槽位首地址一致，杜绝错传旧槽位或无关内存 */
    if (mbx_out_buf != NULL && mbx_out_buf != slot_buf) {
        foe_neutralize_slot_if_ack(slot_buf, adapter->mbx_ctx.buffer_size);
        adapter->has_staged_wrq = false;
        adapter->staged_filename[0] = '\0';
        adapter->staged_target_id = 0;
        return -1; /* 槽位不匹配，安全中和并报错 */
    }

    /* 对新认领槽位中的报文进行长度与格式校验 */
    uint16_t mbx_len = (uint16_t)(slot_buf[0] | ((uint16_t)slot_buf[1] << 8));
    uint8_t mbx_type = (uint8_t)(slot_buf[5] & 0x0FU);

    if (mbx_len != ECAT_FOE_HDR_SIZE ||
        adapter->mbx_ctx.buffer_size < (ECAT_MBX_HDR_SIZE + mbx_len) ||
        (mbx_out_buf != NULL && mbx_out_len < (size_t)(ECAT_MBX_HDR_SIZE + mbx_len)) ||
        mbx_type != ECAT_MBX_TYPE_FOE) {
        foe_neutralize_slot_if_ack(slot_buf, adapter->mbx_ctx.buffer_size);
        adapter->has_staged_wrq = false;
        adapter->staged_filename[0] = '\0';
        adapter->staged_target_id = 0;
        return -1; /* 报文格式或类型异常，安全中和并报错 */
    }

    uint8_t opcode = slot_buf[ECAT_MBX_HDR_SIZE];
    uint32_t packet_num = (uint32_t)(slot_buf[ECAT_MBX_HDR_SIZE + 2] |
                                     ((uint32_t)slot_buf[ECAT_MBX_HDR_SIZE + 3] << 8) |
                                     ((uint32_t)slot_buf[ECAT_MBX_HDR_SIZE + 4] << 16) |
                                     ((uint32_t)slot_buf[ECAT_MBX_HDR_SIZE + 5] << 24));

    if (opcode == ECAT_FOE_OP_ACK && packet_num == 0) {
        /* SOES 原生校验通过且已将有效 ACK(0) 置入发送邮箱待发：尝试正式提交核心会话 */
        int rc = ecat_foe_core_on_wrq(adapter->core,
                                      adapter->staged_filename,
                                      strlen(adapter->staged_filename));
        adapter->has_staged_wrq = false;
        adapter->staged_filename[0] = '\0';
        adapter->staged_target_id = 0;

        if (rc != 0) {
            /* 核心激活失败：必须将该槽位就地改写为 FOE_OP_ERR，杜绝主站收到假 ACK(0) */
            foe_convert_slot_to_error(slot_buf, adapter->mbx_ctx.buffer_size, ECAT_FOE_ERR_NOTDEFINED);
            return -1;
        }

        /* 核心激活成功：保留原 ACK(0) 供后续 ESC_writembx 发送给主站 */
        return 0;
    } else if (opcode == ECAT_FOE_OP_ERR) {
        /* SOES 拒绝了 WRQ (如 FOE_OP_ERR 0x800A 密码错误，或 0x8008 非 BOOT 态)：
         * 安全丢弃暂存 WRQ，核心维持原有会话状态不变，且保留 FOE_OP_ERR 以供发给主站 */
        adapter->has_staged_wrq = false;
        adapter->staged_filename[0] = '\0';
        adapter->staged_target_id = 0;
        return 0;
    } else {
        /* 既非有效 ACK(0) 亦非原生 ERR：未知异常，安全中和并报错 */
        foe_neutralize_slot_if_ack(slot_buf, adapter->mbx_ctx.buffer_size);
        adapter->has_staged_wrq = false;
        adapter->staged_filename[0] = '\0';
        adapter->staged_target_id = 0;
        return -1;
    }
}
