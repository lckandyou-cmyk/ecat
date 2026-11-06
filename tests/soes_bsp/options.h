/**
 * @file options.h
 * @brief SOES 宿主测试环境配置参数头文件
 */

#ifndef __SOES_BSP_OPTIONS_H__
#define __SOES_BSP_OPTIONS_H__

#ifndef MBXSIZE
#define MBXSIZE 128
#endif

#ifndef MBXSIZEBOOT
#define MBXSIZEBOOT 128
#endif

#ifndef MBXBUFFERS
#define MBXBUFFERS 4
#endif

#ifndef MBX0_sma
#define MBX0_sma 0x1000
#endif
#ifndef MBX0_sml
#define MBX0_sml 128
#endif
#ifndef MBX0_sme
#define MBX0_sme 0x107F
#endif
#ifndef MBX0_smc
#define MBX0_smc 0x26
#endif

#ifndef MBX1_sma
#define MBX1_sma 0x1080
#endif
#ifndef MBX1_sml
#define MBX1_sml 128
#endif
#ifndef MBX1_sme
#define MBX1_sme 0x10FF
#endif
#ifndef MBX1_smc
#define MBX1_smc 0x22
#endif

#ifndef MBX0_sma_b
#define MBX0_sma_b 0x1000
#endif
#ifndef MBX0_sml_b
#define MBX0_sml_b 128
#endif
#ifndef MBX0_sme_b
#define MBX0_sme_b 0x107F
#endif
#ifndef MBX0_smc_b
#define MBX0_smc_b 0x26
#endif

#ifndef MBX1_sma_b
#define MBX1_sma_b 0x1080
#endif
#ifndef MBX1_sml_b
#define MBX1_sml_b 128
#endif
#ifndef MBX1_sme_b
#define MBX1_sme_b 0x10FF
#endif
#ifndef MBX1_smc_b
#define MBX1_smc_b 0x22
#endif

#ifndef SM2_sma
#define SM2_sma 0x1000
#endif
#ifndef SM2_smc
#define SM2_smc 0x24
#endif
#ifndef SM2_act
#define SM2_act 0x01
#endif
#ifndef SM3_sma
#define SM3_sma 0x1400
#endif
#ifndef SM3_smc
#define SM3_smc 0x22
#endif
#ifndef SM3_act
#define SM3_act 0x01
#endif

#ifndef MAX_MAPPINGS_SM2
#define MAX_MAPPINGS_SM2 1
#endif

#ifndef MAX_MAPPINGS_SM3
#define MAX_MAPPINGS_SM3 1
#endif

#ifndef RX_PDO_OBJIDX
#define RX_PDO_OBJIDX 0x1600
#endif

#ifndef TX_PDO_OBJIDX
#define TX_PDO_OBJIDX 0x1A00
#endif

#ifndef PREALLOC_FACTOR
#define PREALLOC_FACTOR 1
#endif

#endif /* __SOES_BSP_OPTIONS_H__ */
