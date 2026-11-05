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

#define ECAT_TRANSITION_BOOT_TO_INIT    ((uint8_t)((ECAT_STATE_INIT << 4) | ECAT_STATE_BOOT)) /**< 0x13U: BOOT->INIT 迁移码 */
#define ECAT_AL_STATUS_BOOT_ERROR       ((uint8_t)(ECAT_STATE_BOOT | ECAT_STATE_FLAG_ERROR))  /**< 0x13U: BOOT+Error 状态值 */
#define ECAT_AL_STATUS_CODE_NO_VALID_FW 0x0014U                                               /**< ALERR_NOVALIDFIRMWARE (0x0014) */

/**
 * @brief AL 错误码上报函数原型 (在真实 SOES 中对应 ESC_ALerror(uint16_t errornumber))
 */
typedef void (*ecat_foe_al_error_fn_t)(uint16_t errornumber);

typedef struct {
    ecat_foe_core_t        *core;
    uint32_t                buffer_size;        /**< SOES 的 foe_cfg->buffer_size */
    ecat_foe_al_error_fn_t  al_error_fn;        /**< 注入的 AL 错误设置回调 (真实环境接入 ESC_ALerror，单测接入测试桩) */
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
 * 2. 提取动态文件名，调用核心机 ecat_foe_core_on_wrq 进行项目名、目标 ID 与版本防回滚校验；
 * 3. 校验通过后，将文件名安全暂存至适配层静态缓冲区，并同步更新对应目标的 foe_cfg->files[idx].name；
 * 4. 校验失败时，保持原文件名配置不予同步，放行原生 SOES 自动向主站回复 0x8001 Abort；
 * 5. 若此前有未结束残留会话，触发核心自愈并重新建立会话；
 * 6. 原生 SOES 密码校验与 BOOT 态检查在放行后原样生效。
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

#ifdef __cplusplus
}
#endif

#endif /* ECAT_FOE_SOES_H */
