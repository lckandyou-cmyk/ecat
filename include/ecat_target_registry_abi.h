/* Canonical ABI symbol from evaluated target registry configuration values. */
#ifndef ECAT_TARGET_REGISTRY_ABI_H
#define ECAT_TARGET_REGISTRY_ABI_H

#if ECAT_TARGET_REGISTRY_MAX_BLOCKS < 1 || ECAT_TARGET_REGISTRY_MAX_BLOCKS > 65535U
#error "ECAT_TARGET_REGISTRY_MAX_BLOCKS must fit a nonzero uint16_t"
#endif
#if ECAT_TARGET_REGISTRY_MAX_ACL_RULES < 1 || ECAT_TARGET_REGISTRY_MAX_ACL_RULES > 65535U
#error "ECAT_TARGET_REGISTRY_MAX_ACL_RULES must fit a nonzero uint16_t"
#endif

#if (ECAT_TARGET_REGISTRY_MAX_BLOCKS) & (1UL << 15)
#define ECAT_REG_ABI_B15 1
#else
#define ECAT_REG_ABI_B15 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_BLOCKS) & (1UL << 14)
#define ECAT_REG_ABI_B14 1
#else
#define ECAT_REG_ABI_B14 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_BLOCKS) & (1UL << 13)
#define ECAT_REG_ABI_B13 1
#else
#define ECAT_REG_ABI_B13 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_BLOCKS) & (1UL << 12)
#define ECAT_REG_ABI_B12 1
#else
#define ECAT_REG_ABI_B12 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_BLOCKS) & (1UL << 11)
#define ECAT_REG_ABI_B11 1
#else
#define ECAT_REG_ABI_B11 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_BLOCKS) & (1UL << 10)
#define ECAT_REG_ABI_B10 1
#else
#define ECAT_REG_ABI_B10 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_BLOCKS) & (1UL << 9)
#define ECAT_REG_ABI_B9 1
#else
#define ECAT_REG_ABI_B9 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_BLOCKS) & (1UL << 8)
#define ECAT_REG_ABI_B8 1
#else
#define ECAT_REG_ABI_B8 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_BLOCKS) & (1UL << 7)
#define ECAT_REG_ABI_B7 1
#else
#define ECAT_REG_ABI_B7 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_BLOCKS) & (1UL << 6)
#define ECAT_REG_ABI_B6 1
#else
#define ECAT_REG_ABI_B6 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_BLOCKS) & (1UL << 5)
#define ECAT_REG_ABI_B5 1
#else
#define ECAT_REG_ABI_B5 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_BLOCKS) & (1UL << 4)
#define ECAT_REG_ABI_B4 1
#else
#define ECAT_REG_ABI_B4 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_BLOCKS) & (1UL << 3)
#define ECAT_REG_ABI_B3 1
#else
#define ECAT_REG_ABI_B3 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_BLOCKS) & (1UL << 2)
#define ECAT_REG_ABI_B2 1
#else
#define ECAT_REG_ABI_B2 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_BLOCKS) & (1UL << 1)
#define ECAT_REG_ABI_B1 1
#else
#define ECAT_REG_ABI_B1 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_BLOCKS) & (1UL << 0)
#define ECAT_REG_ABI_B0 1
#else
#define ECAT_REG_ABI_B0 0
#endif

#if (ECAT_TARGET_REGISTRY_MAX_ACL_RULES) & (1UL << 15)
#define ECAT_REG_ABI_A15 1
#else
#define ECAT_REG_ABI_A15 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_ACL_RULES) & (1UL << 14)
#define ECAT_REG_ABI_A14 1
#else
#define ECAT_REG_ABI_A14 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_ACL_RULES) & (1UL << 13)
#define ECAT_REG_ABI_A13 1
#else
#define ECAT_REG_ABI_A13 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_ACL_RULES) & (1UL << 12)
#define ECAT_REG_ABI_A12 1
#else
#define ECAT_REG_ABI_A12 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_ACL_RULES) & (1UL << 11)
#define ECAT_REG_ABI_A11 1
#else
#define ECAT_REG_ABI_A11 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_ACL_RULES) & (1UL << 10)
#define ECAT_REG_ABI_A10 1
#else
#define ECAT_REG_ABI_A10 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_ACL_RULES) & (1UL << 9)
#define ECAT_REG_ABI_A9 1
#else
#define ECAT_REG_ABI_A9 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_ACL_RULES) & (1UL << 8)
#define ECAT_REG_ABI_A8 1
#else
#define ECAT_REG_ABI_A8 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_ACL_RULES) & (1UL << 7)
#define ECAT_REG_ABI_A7 1
#else
#define ECAT_REG_ABI_A7 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_ACL_RULES) & (1UL << 6)
#define ECAT_REG_ABI_A6 1
#else
#define ECAT_REG_ABI_A6 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_ACL_RULES) & (1UL << 5)
#define ECAT_REG_ABI_A5 1
#else
#define ECAT_REG_ABI_A5 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_ACL_RULES) & (1UL << 4)
#define ECAT_REG_ABI_A4 1
#else
#define ECAT_REG_ABI_A4 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_ACL_RULES) & (1UL << 3)
#define ECAT_REG_ABI_A3 1
#else
#define ECAT_REG_ABI_A3 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_ACL_RULES) & (1UL << 2)
#define ECAT_REG_ABI_A2 1
#else
#define ECAT_REG_ABI_A2 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_ACL_RULES) & (1UL << 1)
#define ECAT_REG_ABI_A1 1
#else
#define ECAT_REG_ABI_A1 0
#endif
#if (ECAT_TARGET_REGISTRY_MAX_ACL_RULES) & (1UL << 0)
#define ECAT_REG_ABI_A0 1
#else
#define ECAT_REG_ABI_A0 0
#endif

#define ECAT_TARGET_REGISTRY_ABI_JOIN_IMPL(b15, b14, b13, b12, b11, b10, b9, b8, b7, b6, b5, b4, b3, b2, b1, b0, a15, a14, a13, a12, a11, a10, a9, a8, a7, a6, a5, a4, a3, a2, a1, a0) \
    ecat_target_registry_abi_b##b15##b14##b13##b12##b11##b10##b9##b8##b7##b6##b5##b4##b3##b2##b1##b0##_a##a15##a14##a13##a12##a11##a10##a9##a8##a7##a6##a5##a4##a3##a2##a1##a0

#define ECAT_TARGET_REGISTRY_ABI_JOIN(b15, b14, b13, b12, b11, b10, b9, b8, b7, b6, b5, b4, b3, b2, b1, b0, a15, a14, a13, a12, a11, a10, a9, a8, a7, a6, a5, a4, a3, a2, a1, a0) \
    ECAT_TARGET_REGISTRY_ABI_JOIN_IMPL(b15, b14, b13, b12, b11, b10, b9, b8, b7, b6, b5, b4, b3, b2, b1, b0, a15, a14, a13, a12, a11, a10, a9, a8, a7, a6, a5, a4, a3, a2, a1, a0)

#define ECAT_TARGET_REGISTRY_ABI_SYMBOL ECAT_TARGET_REGISTRY_ABI_JOIN( \
    ECAT_REG_ABI_B15, ECAT_REG_ABI_B14, ECAT_REG_ABI_B13, ECAT_REG_ABI_B12, \
    ECAT_REG_ABI_B11, ECAT_REG_ABI_B10, ECAT_REG_ABI_B9, ECAT_REG_ABI_B8, \
    ECAT_REG_ABI_B7, ECAT_REG_ABI_B6, ECAT_REG_ABI_B5, ECAT_REG_ABI_B4, \
    ECAT_REG_ABI_B3, ECAT_REG_ABI_B2, ECAT_REG_ABI_B1, ECAT_REG_ABI_B0, \
    ECAT_REG_ABI_A15, ECAT_REG_ABI_A14, ECAT_REG_ABI_A13, ECAT_REG_ABI_A12, \
    ECAT_REG_ABI_A11, ECAT_REG_ABI_A10, ECAT_REG_ABI_A9, ECAT_REG_ABI_A8, \
    ECAT_REG_ABI_A7, ECAT_REG_ABI_A6, ECAT_REG_ABI_A5, ECAT_REG_ABI_A4, \
    ECAT_REG_ABI_A3, ECAT_REG_ABI_A2, ECAT_REG_ABI_A1, ECAT_REG_ABI_A0)

#endif /* ECAT_TARGET_REGISTRY_ABI_H */
