/**
 * @file esc_foe.h
 * @brief 最小 SOES 桩头文件，用于单元测试中与 SOES 适配层编译联调
 *
 * 注释行号对应 SOES 官方源码 soes/esc_foe.h (commit/version 3.1.0):
 * - 行 19-40: foe_file_cfg_t 结构体定义
 * - 行 42-52: foe_cfg_t 结构体定义
 *
 * 偏差风险说明 (Stub vs 真实 SOES):
 * 1. 真实 SOES 包含 <cc.h> 平台宏 (CC_PACKED, ALIGN 等)，不同编译器可能有不同对齐属性；
 * 2. 真实 SOES 的 padding:24 位域在不同大小端/编译器位域布局中可能占位不同；
 * 3. 真实 SOES 宏 FOE_FN_MAX = 31，若用户配置超长文件名需注意截断。
 */

#ifndef TEST_STUBS_ESC_FOE_H
#define TEST_STUBS_ESC_FOE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 对应 soes/esc_foe.h 第 17 行 */
#define FOE_FN_MAX 31

/* 对应 soes/esc_foe.h 第 19-40 行 */
typedef struct foe_file_cfg foe_file_cfg_t;
struct foe_file_cfg {
    const char *name;                                                            /* 行 23 */
    uint32_t    max_data;                                                        /* 行 25 */
    uint32_t    dest_start_address;                                              /* 行 27 */
    uint32_t    address_offset;                                                  /* 行 29 */
    uint32_t    total_size;                                                      /* 行 31 */
    uint32_t    filepass;                                                        /* 行 33 */
    uint8_t     write_only_in_boot;                                              /* 行 35 */
    uint32_t    padding : 24;                                                    /* 行 37 */
    uint32_t  (*write_function)(foe_file_cfg_t *self, uint8_t *data, size_t length); /* 行 39 */
};

/* 对应 soes/esc_foe.h 第 42-52 行 */
typedef struct foe_cfg {
    uint8_t        *fbuffer;     /* 行 45: 聚合接收缓冲区 */
    uint32_t        buffer_size; /* 行 47: 缓冲区大小 */
    uint32_t        n_files;     /* 行 49 */
    foe_file_cfg_t *files;       /* 行 51 */
} foe_cfg_t;

#ifdef __cplusplus
}
#endif

#endif /* TEST_STUBS_ESC_FOE_H */
