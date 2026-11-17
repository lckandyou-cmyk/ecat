/**
 * @file ecat_foe_conf_template.h
 * @brief 全局工程编译期配置模板
 *
 * 用户移植时可将本文件复制为 ecat_foe_conf.h，置于包含路径，并对所有编译单元定义
 * ECAT_FOE_CONFIG_HEADER="ecat_foe_conf.h"；未定义时默认使用本模板。
 * 固件与数据目标统一在 ecat_target_registry_t 中注册，设备身份及安全策略在 ecat_foe_core_cfg_t 中配置。
 * 静态缓冲区与注册表容量宏支持预处理整数常量表达式，所有包含公开头的编译单元求值结果须完全一致；
 * ABI 链接符号按求值结果编码校验，混用不同配置或旧对象文件将在链接期报错阻断。
 * 所有物理基地址与分区容量上限均由板级本地配置裁决，坚决不信任外部报文携带的任何物理地址。
 */

/* A direct template include must honor the same selection as core.h.  The
 * selection guard also permits a user config copied from this template. */
#if defined(ECAT_FOE_CONFIG_HEADER) && !defined(ECAT_FOE_CONFIG_SELECTING)
#define ECAT_FOE_CONFIG_SELECTING
#include ECAT_FOE_CONFIG_HEADER
#undef ECAT_FOE_CONFIG_SELECTING
#else
#ifndef ECAT_FOE_CONF_TEMPLATE_H
#define ECAT_FOE_CONF_TEMPLATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                       编译期静态缓冲区与特性默认开关                       */
/* -------------------------------------------------------------------------- */

/* 最大受支持 EFW 头为 v2 的 148 字节；必须 >= 148 且为 4 的倍数。 */
#define ECAT_FOE_HEADER_BUF_MAX             148U

/* 最大写入对齐单位 (核心机残留缓冲区尺寸，须覆盖所有已注册固件目标的 write_unit) */
#define ECAT_FOE_MAX_WRITE_UNIT             256U

/* 可选：如需调整单一目标注册表槽位数或 ACL 规则上限，可在工程配置头中统一定义：
 * #define ECAT_TARGET_REGISTRY_MAX_BLOCKS     16U
 * #define ECAT_TARGET_REGISTRY_MAX_ACL_RULES  32U
 */

/* 设备端固件签名验证参考开关 (默认关闭；启用时须配置受信公钥表与密码学验签回调，失败关闭) */
#define ECAT_FOE_SIGNATURE_VERIFY_ENABLE    0

/* 防回滚版本下限读取接口 (启用 anti_rollback_enable 时由板级 Platform/Port 实现)。 */
int port_get_version_floor(uint16_t target_id, uint32_t *floor_ver);
/* 独立下限写入接口仅供平台管理或测试使用；升级提交由目标驱动 finalize_with_version_floor 联合事务完成。 */
int port_set_version_floor(uint16_t target_id, uint32_t new_floor_ver);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_FOE_CONF_TEMPLATE_H */
#endif /* configuration selection */
