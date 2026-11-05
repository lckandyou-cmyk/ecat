/**
 * @file cc.h
 * @brief SOES 宿主测试环境编译器与平台控制适配头文件
 */

#ifndef __SOES_BSP_CC_H__
#define __SOES_BSP_CC_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <inttypes.h>

#define EC_LITTLE_ENDIAN 1

#define CC_PACKED __attribute__((packed))
#define CC_PACKED_BEGIN
#define CC_PACKED_END
#define CC_ALIGNED(n) __attribute__((aligned(n)))

#define htoes(x) ((uint16_t)(x))
#define etohs(x) ((uint16_t)(x))
#define htoel(x) ((uint32_t)(x))
#define etohl(x) ((uint32_t)(x))

#define DPRINT(...) do {} while (0)

#endif /* __SOES_BSP_CC_H__ */
