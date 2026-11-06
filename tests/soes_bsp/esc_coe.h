/**
 * @file esc_coe.h
 * @brief SOES 宿主测试环境 CoE 结构桩声明
 */

#ifndef __SOES_BSP_ESC_COE_H__
#define __SOES_BSP_ESC_COE_H__

#include <stdint.h>

typedef struct {
    uint16_t index;
    uint8_t subindex;
} _SMmap;

uint16_t sizeOfPDO (uint16_t objidx, int *mappings, _SMmap *map, int max_mappings);

#endif /* __SOES_BSP_ESC_COE_H__ */
