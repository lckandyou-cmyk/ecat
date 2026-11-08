/**
 * @file bootloader_app.c
 * @brief 最小 Bootloader 核心应用层模块实现
 */

#include "bootloader_app.h"
#include <string.h>

/* 真实 SOES 核心 FoE 处理函数声明 */
extern void ESC_foeprocess(void);

int bootloader_app_init(bootloader_app_t *app,
                        foe_cfg_t *foe_cfg,
                        void *mbxcontrol,
                        uint8_t *mbx,
                        uint8_t num_slots,
                        uint16_t slot_size,
                        uint8_t *storage_buf,
                        uint32_t storage_size,
                        ecat_foe_reset_fn_t reset_fn,
                        ecat_foe_al_error_fn_t al_error_fn)
{
    if (app == NULL || foe_cfg == NULL || mbxcontrol == NULL || mbx == NULL) {
        return -1;
    }

    memset(app, 0, sizeof(*app));

    /* 1. 初始化板级 Flash 目标存储驱动 (严格限定本地基地址) */
    bootloader_flash_driver_init_ctx(&app->flash_drv, storage_buf, storage_size);

    /* 2. 组装核心升级状态机全局静态配置 */
    ecat_foe_core_cfg_t core_cfg;
    memset(&core_cfg, 0, sizeof(core_cfg));
    core_cfg.project_id = BOOTLOADER_PROJECT_ID;
    core_cfg.project_name = BOOTLOADER_PROJECT_NAME;
    core_cfg.hardware_id = BOOTLOADER_HARDWARE_ID;
    core_cfg.local_hw_ver = BOOTLOADER_LOCAL_HW_VER;
    core_cfg.anti_rollback_enable = false;
    core_cfg.signature_verify_enable = (ECAT_FOE_SIGNATURE_VERIFY_ENABLE != 0);
    core_cfg.max_header_size = ECAT_FOE_HEADER_BUF_MAX;

    /* 注册 Target 1 (主 MCU 片内 Flash) */
    core_cfg.targets[0].target_id = BOOTLOADER_TARGET_MCU_ID;
    core_cfg.targets[0].enabled = true;
    core_cfg.targets[0].max_payload_size = APP_FLASH_MAX_SIZE;
    core_cfg.targets[0].write_unit = APP_FLASH_WRITE_UNIT;
    core_cfg.targets[0].driver = bootloader_flash_driver_get_ops();
    core_cfg.targets[0].user_ctx = &app->flash_drv;

    /* 3. 初始化通用 FoE 升级核心机 */
    if (ecat_foe_core_init(&app->core, &core_cfg) != 0) {
        return -1;
    }

    /* 4. 初始化 SOES 适配层并绑定信箱上下文 (传入 SOES 文件写缓冲容量 foe_cfg->buffer_size) */
    uint32_t foe_buf_size = (foe_cfg != NULL) ? foe_cfg->buffer_size : 0;
    ecat_foe_soes_init(&app->adapter, &app->core, foe_buf_size);
    if (ecat_foe_soes_set_mbx_context(&app->adapter, mbxcontrol, mbx, num_slots, slot_size) != 0) {
        return -1;
    }

    /* 5. 注入协议栈复位回调与 AL 错误处理回调 */
    if (reset_fn != NULL) {
        ecat_foe_soes_set_foe_reset_fn(&app->adapter, reset_fn);
    }
    if (al_error_fn != NULL) {
        ecat_foe_soes_set_al_error_fn(&app->adapter, al_error_fn);
    }

    /* 6. 将板级 Target 1 映射至 SOES 文件表条目 0 */
    if (foe_cfg->n_files > 0 && foe_cfg->files != NULL) {
        foe_cfg->files[0].name = BOOTLOADER_TARGET_MCU_FILENAME;
        foe_cfg->files[0].max_data = APP_FLASH_MAX_SIZE;
        foe_cfg->files[0].write_function = ecat_foe_soes_write_cb;
    }
    if (ecat_foe_soes_set_target_file_map(&app->adapter,
                                          BOOTLOADER_TARGET_MCU_ID,
                                          0) != 0) {
        return -1;
    }

    app->is_initialized = true;
    return 0;
}

int bootloader_app_step(bootloader_app_t *app,
                        foe_cfg_t *foe_cfg,
                        const uint8_t *rx_buf,
                        size_t rx_len,
                        const uint8_t *tx_buf,
                        size_t tx_len,
                        void (*foeprocess_fn)(void))
{
    if (app == NULL || !app->is_initialized || foe_cfg == NULL) {
        return -1;
    }

    /* 阶段 1: 前置嗅探钩子 (只读解析文件名，暂存 WRQ 并做安全预检) */
    if (rx_buf != NULL && rx_len > 0) {
        (void)ecat_foe_soes_pre_process_hook(&app->adapter, foe_cfg, rx_buf, rx_len);
    }

    /* 阶段 2: SOES 原生处理 (执行密码校验、BOOT 态校验并排队响应) */
    if (foeprocess_fn != NULL) {
        foeprocess_fn();
    } else {
        ESC_foeprocess();
    }

    /* 阶段 3: 后置一致性与提交钩子 (核验响应，异常时就地中和为 0x8000 ERR 并触发 FOE_init 复位) */
    return ecat_foe_soes_post_process_hook(&app->adapter, foe_cfg, tx_buf, tx_len);
}

void bootloader_app_abort(bootloader_app_t *app)
{
    if (app != NULL && app->is_initialized) {
        ecat_foe_core_on_abort(&app->core);
    }
}
