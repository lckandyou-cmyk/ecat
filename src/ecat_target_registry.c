/**
 * @file ecat_target_registry.c
 * @brief 嵌入式通用目标区块注册表与权限控制矩阵实现
 */

#include "ecat_target_registry.h"
#include <string.h>

int ecat_target_registry_init(ecat_target_registry_t *reg)
{
    if (reg == NULL) {
        return -1;
    }
    memset(reg, 0, sizeof(*reg));
    return 0;
}

int ecat_target_registry_add(ecat_target_registry_t *reg, const ecat_target_desc_t *desc)
{
    if (reg == NULL || desc == NULL || desc->target_id == 0U || desc->max_size == 0U) {
        return -1;
    }

    if (desc->name[0] == '\0') {
        return -2;
    }

    if (reg->target_count >= ECAT_TARGET_REGISTRY_MAX_BLOCKS) {
        return -3;
    }

    /* 检查 target_id 是否已被注册 */
    if (ecat_target_registry_find_by_id(reg, desc->target_id) != NULL) {
        return -4;
    }

    /* 检查驱动基本有效性 */
    if ((desc->permissions & ECAT_PERM_WRITE) != 0U) {
        if (desc->driver.io.write == NULL || desc->driver.io.erase_to == NULL) {
            return -5;
        }
    }
    if ((desc->permissions & ECAT_PERM_READ) != 0U) {
        if (desc->driver.io.read == NULL) {
            return -6;
        }
    }

    reg->targets[reg->target_count] = *desc;
    /* 确保名称字符串以 NULL 结尾 */
    reg->targets[reg->target_count].name[ECAT_TARGET_NAME_MAX_LEN - 1U] = '\0';
    reg->target_count++;

    return 0;
}

const ecat_target_desc_t *ecat_target_registry_find_by_id(
    const ecat_target_registry_t *reg, uint16_t target_id)
{
    if (reg == NULL || target_id == 0U) {
        return NULL;
    }

    for (size_t i = 0; i < reg->target_count; i++) {
        if (reg->targets[i].target_id == target_id) {
            return &reg->targets[i];
        }
    }

    return NULL;
}

const ecat_target_desc_t *ecat_target_registry_find_by_name(
    const ecat_target_registry_t *reg, const char *name)
{
    if (reg == NULL || name == NULL || name[0] == '\0') {
        return NULL;
    }

    for (size_t i = 0; i < reg->target_count; i++) {
        if (strncmp(reg->targets[i].name, name, ECAT_TARGET_NAME_MAX_LEN) == 0) {
            return &reg->targets[i];
        }
    }

    return NULL;
}

int ecat_target_registry_add_acl(ecat_target_registry_t *reg,
    uint16_t transport_id, uint16_t target_id, uint8_t allowed_perms)
{
    if (reg == NULL || allowed_perms == 0U) {
        return -1;
    }

    if (reg->acl_rule_count >= ECAT_TARGET_REGISTRY_MAX_ACL_RULES) {
        return -2;
    }

    reg->acl_rules[reg->acl_rule_count].transport_id = transport_id;
    reg->acl_rules[reg->acl_rule_count].target_id = target_id;
    reg->acl_rules[reg->acl_rule_count].allowed_perms = allowed_perms;
    reg->acl_rule_count++;

    return 0;
}

void ecat_target_registry_set_enforce_acl(ecat_target_registry_t *reg, bool enforce)
{
    if (reg != NULL) {
        reg->enforce_acl = enforce;
    }
}

bool ecat_target_registry_check_permission(
    const ecat_target_registry_t *reg,
    uint16_t transport_id,
    uint16_t target_id,
    uint8_t required_perm)
{
    if (reg == NULL || target_id == 0U || required_perm == 0U) {
        return false;
    }

    const ecat_target_desc_t *target = ecat_target_registry_find_by_id(reg, target_id);
    if (target == NULL || !target->enabled) {
        return false;
    }

    /* 首先检查目标本身是否具有该能力 */
    if ((target->permissions & required_perm) != required_perm) {
        return false;
    }

    /* 如果没有开启强制 ACL 过滤，则默认允许目标自身具有的权限 */
    if (!reg->enforce_acl) {
        return true;
    }

    /* 匹配 ACL 白名单规则 */
    for (size_t i = 0; i < reg->acl_rule_count; i++) {
        const ecat_target_acl_rule_t *rule = &reg->acl_rules[i];
        bool transport_match = (rule->transport_id == 0U || rule->transport_id == transport_id);
        bool target_match = (rule->target_id == 0U || rule->target_id == target_id);

        if (transport_match && target_match) {
            if ((rule->allowed_perms & required_perm) == required_perm) {
                return true;
            }
        }
    }

    return false;
}
