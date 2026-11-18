/**
 * @file ecat_target_registry.c
 * @brief 嵌入式通用目标区块注册表与权限控制矩阵实现
 */

#include "ecat_target_registry.h"
#include <string.h>

void ECAT_TARGET_REGISTRY_ABI_SYMBOL(void)
{
}

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

    if (desc->type != ECAT_TARGET_TYPE_FIRMWARE && desc->type != ECAT_TARGET_TYPE_DATA) {
        return -1;
    }

    if ((desc->permissions & ~ECAT_PERM_RW) != 0U || (desc->permissions & ECAT_PERM_RW) == 0U) {
        return -1;
    }

    if (desc->write_unit != 0U && (desc->max_size % desc->write_unit) != 0U) {
        return -1;
    }

    if (desc->name[0] == '\0' || memchr(desc->name, '\0', ECAT_TARGET_NAME_MAX_LEN) == NULL) {
        return -2;
    }

    if (reg->target_count >= ECAT_TARGET_REGISTRY_MAX_BLOCKS) {
        return -3;
    }

    /* 检查 target_id 与 name 是否已被注册 (严格保持双键唯一) */
    if (ecat_target_registry_find_by_id(reg, desc->target_id) != NULL ||
        ecat_target_registry_find_by_name(reg, desc->name) != NULL) {
        return -4;
    }

    /* 若显式提供异步数据驱动或异步固件驱动，所有核心回调 (含 abort) 必须完整 */
    if (desc->driver.async_ops != NULL) {
        if (desc->driver.async_ops->start == NULL ||
            desc->driver.async_ops->poll == NULL ||
            desc->driver.async_ops->abort == NULL) {
            return -5;
        }
    }
    if (desc->driver.fw_async_ops != NULL) {
        if (desc->driver.fw_async_ops->start == NULL ||
            desc->driver.fw_async_ops->poll == NULL ||
            desc->driver.fw_async_ops->abort == NULL ||
            desc->driver.fw_async_ops->abort_session == NULL) {
            return -5;
        }
    }

    /* 检查读写权限对应的驱动基本有效性 */
    if ((desc->permissions & ECAT_PERM_WRITE) != 0U) {
        bool async_fw_write = (desc->type == ECAT_TARGET_TYPE_FIRMWARE && desc->asynchronous && desc->driver.fw_async_ops != NULL);
        bool async_data_write = (desc->asynchronous && desc->driver.async_ops != NULL);
        if (!async_fw_write && !async_data_write) {
            if (desc->driver.io.write == NULL || desc->driver.io.erase_to == NULL) {
                return -5;
            }
        }
    }
    if ((desc->permissions & ECAT_PERM_READ) != 0U) {
        bool async_read = (desc->asynchronous && desc->driver.async_ops != NULL);
        if (!async_read && desc->driver.io.read == NULL) {
            return -6;
        }
    }

    reg->targets[reg->target_count] = *desc;
    reg->target_count++;

    return 0;
}

const ecat_target_desc_t *ecat_target_registry_find_slot_by_id(
    const ecat_target_registry_t *reg, uint16_t target_id, size_t *out_slot)
{
    if (reg == NULL || target_id == 0U) {
        return NULL;
    }

    for (size_t i = 0; i < reg->target_count; i++) {
        if (reg->targets[i].target_id == target_id) {
            if (out_slot != NULL) {
                *out_slot = i;
            }
            return &reg->targets[i];
        }
    }

    return NULL;
}

const ecat_target_desc_t *ecat_target_registry_find_by_id(
    const ecat_target_registry_t *reg, uint16_t target_id)
{
    return ecat_target_registry_find_slot_by_id(reg, target_id, NULL);
}

const ecat_target_desc_t *ecat_target_registry_find_by_name(
    const ecat_target_registry_t *reg, const char *name)
{
    if (reg == NULL || name == NULL || name[0] == '\0') {
        return NULL;
    }

    for (size_t i = 0; i < reg->target_count; i++) {
        if (strcmp(reg->targets[i].name, name) == 0) {
            return &reg->targets[i];
        }
    }

    return NULL;
}

int ecat_target_registry_add_acl(ecat_target_registry_t *reg,
    uint16_t transport_id, uint16_t target_id, uint8_t allowed_perms)
{
    if (reg == NULL || allowed_perms == 0U || (allowed_perms & ~ECAT_PERM_RW) != 0U) {
        return -1;
    }

    /* 检查是否已有相同通道与目标规则，若存在则合并权限 */
    for (size_t i = 0; i < reg->acl_rule_count; i++) {
        if (reg->acl_rules[i].transport_id == transport_id &&
            reg->acl_rules[i].target_id == target_id) {
            reg->acl_rules[i].allowed_perms |= allowed_perms;
            return 0;
        }
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
    if (reg == NULL || target_id == 0U || required_perm == 0U ||
        (required_perm & ~ECAT_PERM_RW) != 0U) {
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

    /* 匹配并聚合所有适用的 ACL 白名单规则 */
    uint8_t effective_perms = 0U;
    for (size_t i = 0; i < reg->acl_rule_count; i++) {
        const ecat_target_acl_rule_t *rule = &reg->acl_rules[i];
        bool transport_match = (rule->transport_id == 0U || rule->transport_id == transport_id);
        bool target_match = (rule->target_id == 0U || rule->target_id == target_id);

        if (transport_match && target_match) {
            effective_perms |= rule->allowed_perms;
        }
    }

    return ((effective_perms & required_perm) == required_perm);
}
