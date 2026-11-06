/**
 * @file ecat_foe_soes.h
 * @brief SOES 协议栈适配层接口定义
 */

#ifndef ECAT_FOE_SOES_H
#define ECAT_FOE_SOES_H

#include "ecat_foe_core.h"
#include "esc_foe.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief EtherCAT AL 状态码与状态迁移常量定义
 *
 * 语义辨析与设计说明：
 * - 迁移码 ECAT_TRANSITION_BOOT_TO_INIT ((ESCinit << 4) | ESCboot = 0x13)：
 *   在 SOES esc.c:1128 中计算所得由 BOOT (0x03) 迁往 INIT (0x01) 的复合事件码。
 * - 状态码 ECAT_AL_STATUS_BOOT_ERROR (ESCboot | ESCerror = 0x13)：
 *   写入 AL 状态寄存器 (0x130) 的状态值，表示当前处于 BOOT 态且 Error 标志位置 1 (0x03 | 0x10)。
 * 两者数值虽同为 0x13，但属于完全不同的寄存器语义和控制逻辑，必须使用明确的命名常量严格区分。
 */
#define ECAT_STATE_INIT                 0x01U
#define ECAT_STATE_PREOP                0x02U
#define ECAT_STATE_BOOT                 0x03U
#define ECAT_STATE_SAFEOP               0x04U
#define ECAT_STATE_OP                   0x08U
#define ECAT_STATE_FLAG_ERROR           0x10U

#ifndef ECAT_FOE_SOES_MAX_FILES
#define ECAT_FOE_SOES_MAX_FILES         4U
#endif

#define ECAT_MBX_HDR_SIZE               6U
#define ECAT_FOE_HDR_SIZE               6U

#define ECAT_TRANSITION_BOOT_TO_INIT    ((uint8_t)((ECAT_STATE_INIT << 4) | ECAT_STATE_BOOT)) /**< 0x13U: BOOT->INIT 迁移码 */
#define ECAT_AL_STATUS_BOOT_ERROR       ((uint8_t)(ECAT_STATE_BOOT | ECAT_STATE_FLAG_ERROR))  /**< 0x13U: BOOT+Error 状态值 */
#define ECAT_AL_STATUS_CODE_NO_VALID_FW 0x0014U                                               /**< ALERR_NOVALIDFIRMWARE (0x0014) */

/**
 * @brief AL 错误码上报函数原型 (在真实 SOES 中对应 ESC_ALerror(uint16_t errornumber))
 */
typedef void (*ecat_foe_al_error_fn_t)(uint16_t errornumber);

#ifndef MBXstate_idle
#define MBXstate_idle                   0x00
#define MBXstate_inclaim                0x01
#define MBXstate_outclaim               0x02
#define MBXstate_outreq                 0x03
#define MBXstate_outpost                0x04
#define MBXstate_backup                 0x05
#define MBXstate_again                  0x06
#endif

#define ECAT_FOE_SOES_MAX_MBX_BUFFERS   16U

/**
 * @brief SOES 邮箱分配与状态只读观察上下文
 */
typedef struct {
    const uint8_t *mbx_control_states; /**< 指向 SOES MBXcontrol[].state 数组首地址 (每个元素 1 字节) */
    const uint8_t *mbx_buffers;        /**< 指向 SOES 全局 MBX 缓冲区首地址 */
    uint8_t        num_buffers;        /**< 邮箱缓冲区总数 (对应 SOES ESC_MBXBUFFERS，通常为 2 或 4) */
    uint16_t       buffer_size;        /**< 单个邮箱槽位字节容量 (对应 SOES ESC_MBXSIZE) */
    uint8_t        pre_states[ECAT_FOE_SOES_MAX_MBX_BUFFERS]; /**< pre_process_hook 执行前各槽位状态快照 */
    int8_t         newly_claimed_slot; /**< 本轮由 SOES 新认领并置为 outreq 的槽位索引 (-1 表示未认领) */
} ecat_foe_soes_mbx_ctx_t;

typedef struct {
    ecat_foe_core_t        *core;
    uint32_t                buffer_size;        /**< SOES 的 foe_cfg->buffer_size */
    ecat_foe_al_error_fn_t  al_error_fn;        /**< 注入的 AL 错误设置回调 (真实环境接入 ESC_ALerror，单测接入测试桩) */
    bool                    has_staged_wrq;     /**< 是否存在已通过只读预检但尚未确认接受的 WRQ */
    char                    staged_filename[FOE_FN_MAX + 1]; /**< 暂存的 WRQ 文件名 */
    uint16_t                staged_target_id;   /**< 暂存的目标 ID */
    uint32_t                original_n_files;   /**< 从站初始配置的真实文件条目数 */
    int8_t                  target_to_file_map[ECAT_FOE_TARGET_COUNT + 1]; /**< 目标 ID (1..4) 到 files[] 索引映射 (-1 为未映射) */
    ecat_foe_soes_mbx_ctx_t mbx_ctx;            /**< SOES 邮箱分配与状态观察上下文 (不篡改 SOES 内存所有权) */
} ecat_foe_soes_adapter_t;

/**
 * @brief 初始化 SOES 适配层
 * @param adapter 适配层上下文
 * @param core    核心升级状态机实例
 * @param buffer_size SOES 配置的 fbuffer 尺寸
 */
void ecat_foe_soes_init(ecat_foe_soes_adapter_t *adapter, ecat_foe_core_t *core, uint32_t buffer_size);

/**
 * @brief 设置 AL 错误上报注入回调函数 (真实 SOES 中传入 ESC_ALerror，单测传入桩函数)
 * @param adapter 适配层上下文
 * @param al_error_fn 错误上报函数指针
 */
void ecat_foe_soes_set_al_error_fn(ecat_foe_soes_adapter_t *adapter, ecat_foe_al_error_fn_t al_error_fn);

/**
 * @brief 重置目标 ID 到 SOES 文件表索引的映射为全部未映射 (-1)
 *
 * 适用于配置稀疏或紧凑文件表前清空默认 1:1 映射，避免与既有条目发生映射冲突。
 *
 * @param adapter 适配层上下文指针
 */
void ecat_foe_soes_reset_target_file_map(ecat_foe_soes_adapter_t *adapter);

/**
 * @brief 配置目标 ID 到 SOES 文件表索引的映射关系 (支持稀疏配置与紧凑文件表)
 *
 * 默认情况下，适配层采用 1:1 直接映射 (目标 1 -> files[0], 目标 2 -> files[1] 等)。
 * 当从站启用稀疏目标 (例如仅使能目标 2 与 4，且 files[] 仅声明 2 个条目) 时，
 * 可先调用 ecat_foe_soes_reset_target_file_map 清空默认映射，再通过此接口显式指定映射。
 * 若传入 file_idx < 0 则将该目标置为未映射。
 *
 * @param adapter   适配层上下文指针
 * @param target_id 目标 ID (1..ECAT_FOE_TARGET_COUNT)
 * @param file_idx  SOES foe_cfg->files[] 数组索引 (若为 -1 表示取消映射)
 * @return 0 成功；非 0 表示参数非法或发生重复映射冲突
 */
int ecat_foe_soes_set_target_file_map(ecat_foe_soes_adapter_t *adapter, uint16_t target_id, int32_t file_idx);

/**
 * @brief 配置 SOES 邮箱分配与状态只读观察上下文
 *
 * 依据真实 SOES 邮箱分配机制 (ESC_claimbuffer / MBXcontrol)：
 * 适配层严格遵循只读观察原则，不篡改 SOES 内存所有权，不向发送邮箱缓冲区或状态数组写入任何数据。
 * 在 pre_process_hook 执行时对各槽位状态进行快照；
 * 在 post_process_hook 执行时比对状态迁移 (仅当槽位由 MBXstate_idle 跃迁至 MBXstate_outreq 时认定为本轮新响应)；
 * 响应报文完整保留在 SOES 缓冲区内以供后续 ESC_writembx 正常发送给主站；
 * 若未配置邮箱上下文，或出现无新响应、多槽或非 idle 状态，适配层严格遵循失败关闭 (Fail-Closed) 原则，绝不激活会话。
 *
 * @param adapter       适配层上下文指针
 * @param mbx_control   指向 SOES MBXcontrol[0].state 数组首地址
 * @param mbx_buffers   指向 SOES MBX 全局缓冲区首地址
 * @param num_buffers   邮箱缓冲区总数 (ESC_MBXBUFFERS，至少为 2，且不超过 ECAT_FOE_SOES_MAX_MBX_BUFFERS)
 * @param buffer_size   单个邮箱槽位字节容量 (ESC_MBXSIZE，至少 12 字节)
 * @return 0 成功；非 0 表示参数非法
 */
int ecat_foe_soes_set_mbx_context(ecat_foe_soes_adapter_t *adapter,
                                  const void *mbx_control,
                                  const uint8_t *mbx_buffers,
                                  uint8_t num_buffers,
                                  uint16_t buffer_size);

/**
 * @brief 获取最近一轮由 SOES 成功认领并产出响应的邮箱槽位索引
 *
 * @param adapter 适配层上下文指针
 * @return 槽位索引 (1..num_buffers-1)；若无新槽位认领则返回 -1
 */
int8_t ecat_foe_soes_get_newly_claimed_slot(const ecat_foe_soes_adapter_t *adapter);

/**
 * @brief SOES WRQ 检查钩子 (在主站发起 WRQ 时调用)
 * @param adapter      适配层上下文
 * @param filename     文件名缓冲区
 * @param filename_len 文件名长度 (字节)
 * @return 0 成功，非 0 失败 (返回 FoE 错误码)
 */
int ecat_foe_soes_on_wrq(ecat_foe_soes_adapter_t *adapter, const char *filename, size_t filename_len);

/**
 * @brief SOES 写入回调函数 (绑定至 foe_file_cfg_t 的 write_function)
 * 在 SOES 上，数据包与短包的结束路径返回非零会触发 FOE_ERR_PROGERROR；零长度结束包路径忽略返回值，失败不会传给主站。
 * @return 0 成功；非 0 失败 (触发 SOES 下发 FOE_ERR_PROGERROR)
 */
uint32_t ecat_foe_soes_write_cb(foe_file_cfg_t *self, uint8_t *data, size_t length);

/**
 * @brief SOES 状态迁移拦截钩子 (post_state_change_hook)
 * 在 BOOT->INIT 迁移时，若最后提交失败，改写 *an 为 BOOT|Error 并写入错误码 0x0014
 */
void ecat_foe_soes_post_state_hook(ecat_foe_soes_adapter_t *adapter, uint8_t *as, uint8_t *an);

/**
 * @brief SOES 邮箱前置嗅探与动态文件名同步钩子 (Mailbox Sniffer Hook)
 *
 * 在 PDI 主循环调用 ESC_foeprocess() 之前调用本函数。
 * 若检测到主站下发 FOE_OP_WRQ：
 * 1. 严格核验 EtherCAT 邮箱帧边界 (mbx_buf_len >= 12 字节，包含 6 字节 Mailbox 头与 6 字节 FoE 头)；
 * 2. 提取动态文件名，调用核心只读预检函数 ecat_foe_core_validate_wrq 校验项目名、格式与目标使能状态 (防回滚在后续数据头校验)；
 * 3. 校验通过后，根据目标映射表同步更新对应条目的 foe_cfg->files[file_idx].name，并安全暂存该 WRQ；
 * 4. 校验失败或映射越界时，将 foe_cfg->n_files 临时置 0 切断原生比对，使原生 SOES 自动回复 0x8001 Abort；
 * 5. 原生 SOES 密码校验与 BOOT 态检查在放行后由原生协议栈原样执行。
 *
 * @param adapter     适配层上下文指针
 * @param foe_cfg     SOES 的 foe_cfg 配置结构体指针
 * @param mbx_buf     从站 SM2 接收邮箱缓冲区首地址 (通常为 &MBX[0])
 * @param mbx_buf_len 接收邮箱有效数据长度
 * @return 0 成功处理或无需拦截；非 0 表示核心机检验拒绝
 */
int ecat_foe_soes_pre_process_hook(ecat_foe_soes_adapter_t *adapter,
                                   foe_cfg_t *foe_cfg,
                                   const uint8_t *mbx_buf,
                                   size_t mbx_buf_len);

/**
 * @brief SOES 邮箱后置处理钩子 (Post-Process Hook)
 *
 * 在 PDI 主循环调用 ESC_foeprocess() 之后调用本函数。
 * 1. 恢复 foe_cfg->n_files 为真实原始条目数；
 * 2. 若存在暂存的 WRQ：
 *    - 严格核验从站发送邮箱是否为有效 FoE ACK(0) 报文 (mbxtype == 0x04, length >= 6, opcode == 0x04, packetnumber == 0)；
 *    - 若核验通过：确认原生 SOES 已通过密码/BOOT等全部检查并正式接受 WRQ，正式调用 core_on_wrq 激活新会话；
 *    - 若核验未通过 (包括 FOE_OP_ERR、NULL 缓冲区、空帧/短帧、非 FoE 报文等)：
 *      说明原生 SOES 拒绝了该 WRQ 或报文无关，安全丢弃暂存 WRQ，绝不打断核心旧会话。
 *
 * @param adapter      适配层上下文指针
 * @param foe_cfg      SOES 的 foe_cfg 配置结构体指针
 * @param mbx_out_buf  从站 SM3 发送邮箱缓冲区首地址 (通常为 &MBX[1 * ESC_MBXSIZE] 或实际输出邮箱)
 * @param mbx_out_len  发送邮箱数据长度
 * @return 0 成功
 */
int ecat_foe_soes_post_process_hook(ecat_foe_soes_adapter_t *adapter,
                                    foe_cfg_t *foe_cfg,
                                    const uint8_t *mbx_out_buf,
                                    size_t mbx_out_len);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_FOE_SOES_H */
