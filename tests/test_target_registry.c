/**
 * @file test_target_registry.c
 * @brief 目标注册表与权限控制单元测试
 */

#include "ecat_target_registry.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

/* Mock 驱动函数 */
static int mock_init(void *ctx) { (void)ctx; return 0; }
static int mock_invalidate(void *ctx) { (void)ctx; return 0; }
static int mock_erase_to(uint32_t need_end, uint32_t *erased_end, void *ctx)
{
    (void)ctx;
    if (erased_end) *erased_end = need_end;
    return 0;
}
static int mock_write(uint32_t offset, const uint8_t *data, uint32_t len, void *ctx)
{
    (void)offset; (void)data; (void)len; (void)ctx;
    return 0;
}
static int mock_read(uint32_t offset, uint8_t *buf, uint32_t len, void *ctx)
{
    (void)offset; (void)buf; (void)len; (void)ctx;
    return 0;
}
static int mock_finalize(void *ctx) { (void)ctx; return 0; }
static void mock_abort(void *ctx) { (void)ctx; }

int main(void)
{
    printf("=== 开始运行目标注册表与权限控制单元测试 ===\n");

    ecat_target_registry_t reg;
    assert(ecat_target_registry_init(NULL) == -1);
    assert(ecat_target_registry_init(&reg) == 0);
    assert(reg.target_count == 0);

    /* 1. 参数校验测试 */
    ecat_target_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    assert(ecat_target_registry_add(&reg, NULL) == -1);
    assert(ecat_target_registry_add(&reg, &desc) == -1); /* target_id = 0, max_size = 0 */

    desc.target_id = 1U;
    desc.max_size = 1024U * 1024U;
    assert(ecat_target_registry_add(&reg, &desc) == -2); /* name empty */

    strncpy(desc.name, "mcu_app", sizeof(desc.name));
    desc.permissions = ECAT_PERM_WRITE;
    assert(ecat_target_registry_add(&reg, &desc) == -5); /* missing write/erase callbacks */

    desc.driver.io.write = mock_write;
    desc.driver.io.erase_to = mock_erase_to;
    desc.driver.io.read = mock_read;
    desc.driver.io.init = mock_init;
    desc.driver.io.invalidate = mock_invalidate;
    desc.driver.io.finalize = mock_finalize;
    desc.driver.io.abort = mock_abort;
    desc.enabled = true;
    desc.type = ECAT_TARGET_TYPE_FIRMWARE;

    /* 正常注册第一个区块 (MCU 固件区: 仅写) */
    assert(ecat_target_registry_add(&reg, &desc) == 0);
    assert(reg.target_count == 1);

    /* 重复 target_id 注册被拦截 */
    assert(ecat_target_registry_add(&reg, &desc) == -4);

    /* 注册第二个区块 (参数区: 读写 RW) */
    ecat_target_desc_t param_desc = desc;
    param_desc.target_id = 2U;
    strncpy(param_desc.name, "params", sizeof(param_desc.name));
    param_desc.type = ECAT_TARGET_TYPE_DATA;
    param_desc.permissions = ECAT_PERM_RW;
    param_desc.max_size = 64U * 1024U;
    assert(ecat_target_registry_add(&reg, &param_desc) == 0);

    /* 注册第三个区块 (日志区: 仅读 READ) */
    ecat_target_desc_t log_desc = desc;
    log_desc.target_id = 3U;
    strncpy(log_desc.name, "sys_log", sizeof(log_desc.name));
    log_desc.type = ECAT_TARGET_TYPE_DATA;
    log_desc.permissions = ECAT_PERM_READ;
    log_desc.max_size = 256U * 1024U;
    /* 仅读目标即使 write 为 NULL 也应允许 */
    log_desc.driver.io.write = NULL;
    log_desc.driver.io.erase_to = NULL;
    assert(ecat_target_registry_add(&reg, &log_desc) == 0);

    /* 2. 查找功能验证 */
    const ecat_target_desc_t *found = ecat_target_registry_find_by_id(&reg, 1U);
    assert(found != NULL && strcmp(found->name, "mcu_app") == 0);
    assert(found->type == ECAT_TARGET_TYPE_FIRMWARE);

    found = ecat_target_registry_find_by_name(&reg, "params");
    assert(found != NULL && found->target_id == 2U);
    assert(found->type == ECAT_TARGET_TYPE_DATA);

    assert(ecat_target_registry_find_by_id(&reg, 999U) == NULL);
    assert(ecat_target_registry_find_by_name(&reg, "non_existent") == NULL);

    /* 3. 基础权限判断 (enforce_acl = false 模式) */
    /* MCU 固件仅写: 允许 WRITE，拒绝 READ */
    assert(ecat_target_registry_check_permission(&reg, 100U, 1U, ECAT_PERM_WRITE) == true);
    assert(ecat_target_registry_check_permission(&reg, 100U, 1U, ECAT_PERM_READ) == false);

    /* 日志区仅读: 允许 READ，拒绝 WRITE */
    assert(ecat_target_registry_check_permission(&reg, 100U, 3U, ECAT_PERM_READ) == true);
    assert(ecat_target_registry_check_permission(&reg, 100U, 3U, ECAT_PERM_WRITE) == false);

    /* 参数区读写: 允许 READ & WRITE */
    assert(ecat_target_registry_check_permission(&reg, 100U, 2U, ECAT_PERM_READ) == true);
    assert(ecat_target_registry_check_permission(&reg, 100U, 2U, ECAT_PERM_WRITE) == true);

    /* 4. ACL 强制访问控制矩阵 (enforce_acl = true) */
    ecat_target_registry_set_enforce_acl(&reg, true);

    /* 此时没有任何 ACL 规则，全部拒绝 */
    assert(ecat_target_registry_check_permission(&reg, 1U /* FoE */, 1U, ECAT_PERM_WRITE) == false);

    /* 添加规则 1: FoE (ID=1) 允许写 MCU (ID=1) */
    assert(ecat_target_registry_add_acl(&reg, 1U, 1U, ECAT_PERM_WRITE) == 0);
    assert(ecat_target_registry_check_permission(&reg, 1U, 1U, ECAT_PERM_WRITE) == true);
    /* 但 FoE 不能读 MCU */
    assert(ecat_target_registry_check_permission(&reg, 1U, 1U, ECAT_PERM_READ) == false);
    /* 串口 (ID=2) 不能写 MCU */
    assert(ecat_target_registry_check_permission(&reg, 2U, 1U, ECAT_PERM_WRITE) == false);

    /* 添加规则 2: 通配所有传输通道 (ID=0) 允许读取日志区 (ID=3) */
    assert(ecat_target_registry_add_acl(&reg, 0U, 3U, ECAT_PERM_READ) == 0);
    assert(ecat_target_registry_check_permission(&reg, 1U, 3U, ECAT_PERM_READ) == true);
    assert(ecat_target_registry_check_permission(&reg, 2U, 3U, ECAT_PERM_READ) == true);
    assert(ecat_target_registry_check_permission(&reg, 99U, 3U, ECAT_PERM_READ) == true);

    printf(">>> 目标注册表与权限控制单元测试全部通过！<<<\n");
    return 0;
}
