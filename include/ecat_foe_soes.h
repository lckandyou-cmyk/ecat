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
 * @brief AL 错误码上报函数原型 (在真实 SOES 中对应 ESC_ALerror(uint16_t errornumber))
 */
typedef void (*ecat_foe_al_error_fn_t)(uint16_t errornumber);

typedef struct {
    ecat_foe_core_t        *core;
    uint32_t                buffer_size;        /**< SOES 的 foe_cfg->buffer_size */
    bool                    last_commit_failed; /**< 记录最后提交失败状态 (用于 BOOT->INIT 拦截) */
    uint16_t                last_al_error;      /**< 记录设置的 AL 错误码 (如 0x0014 ALERR_NOVALIDFIRMWARE) */
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
 * 以 length < buffer_size 作为结束判定依据 (包含 0 长度包)。
 * @return 0 成功；非 0 失败 (触发 SOES 下发 FOE_ERR_PROGERROR)
 */
uint32_t ecat_foe_soes_write_cb(foe_file_cfg_t *self, uint8_t *data, size_t length);

/**
 * @brief SOES 状态迁移拦截钩子 (post_state_change_hook)
 * 在 BOOT->INIT 迁移时，若最后提交失败，改写 *an 为 BOOT|Error 并写入错误码 0x0014
 */
void ecat_foe_soes_post_state_hook(ecat_foe_soes_adapter_t *adapter, uint8_t *as, uint8_t *an);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_FOE_SOES_H */
