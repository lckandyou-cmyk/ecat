/* Canonical ABI symbol from evaluated configuration values, not macro spelling.
 * Bit expansion is preprocessing only: parentheses, arithmetic, hex and shifts
 * with the same value select the same symbol. Ranges match the public size
 * fields (16-bit header length, 32-bit write unit). */
#ifndef ECAT_FOE_CONFIG_ABI_H
#define ECAT_FOE_CONFIG_ABI_H

#if ECAT_FOE_HEADER_BUF_MAX < 1 || ECAT_FOE_HEADER_BUF_MAX > 65535U
#error "ECAT_FOE_HEADER_BUF_MAX must fit a nonzero uint16_t"
#endif
#if ECAT_FOE_MAX_WRITE_UNIT < 1 || ECAT_FOE_MAX_WRITE_UNIT > 0xFFFFFFFFU
#error "ECAT_FOE_MAX_WRITE_UNIT must fit a nonzero uint32_t"
#endif

#if (ECAT_FOE_HEADER_BUF_MAX) & (1UL << 15)
#define ECAT_FOE_ABI_H15 1
#else
#define ECAT_FOE_ABI_H15 0
#endif
#if (ECAT_FOE_HEADER_BUF_MAX) & (1UL << 14)
#define ECAT_FOE_ABI_H14 1
#else
#define ECAT_FOE_ABI_H14 0
#endif
#if (ECAT_FOE_HEADER_BUF_MAX) & (1UL << 13)
#define ECAT_FOE_ABI_H13 1
#else
#define ECAT_FOE_ABI_H13 0
#endif
#if (ECAT_FOE_HEADER_BUF_MAX) & (1UL << 12)
#define ECAT_FOE_ABI_H12 1
#else
#define ECAT_FOE_ABI_H12 0
#endif
#if (ECAT_FOE_HEADER_BUF_MAX) & (1UL << 11)
#define ECAT_FOE_ABI_H11 1
#else
#define ECAT_FOE_ABI_H11 0
#endif
#if (ECAT_FOE_HEADER_BUF_MAX) & (1UL << 10)
#define ECAT_FOE_ABI_H10 1
#else
#define ECAT_FOE_ABI_H10 0
#endif
#if (ECAT_FOE_HEADER_BUF_MAX) & (1UL << 9)
#define ECAT_FOE_ABI_H9 1
#else
#define ECAT_FOE_ABI_H9 0
#endif
#if (ECAT_FOE_HEADER_BUF_MAX) & (1UL << 8)
#define ECAT_FOE_ABI_H8 1
#else
#define ECAT_FOE_ABI_H8 0
#endif
#if (ECAT_FOE_HEADER_BUF_MAX) & (1UL << 7)
#define ECAT_FOE_ABI_H7 1
#else
#define ECAT_FOE_ABI_H7 0
#endif
#if (ECAT_FOE_HEADER_BUF_MAX) & (1UL << 6)
#define ECAT_FOE_ABI_H6 1
#else
#define ECAT_FOE_ABI_H6 0
#endif
#if (ECAT_FOE_HEADER_BUF_MAX) & (1UL << 5)
#define ECAT_FOE_ABI_H5 1
#else
#define ECAT_FOE_ABI_H5 0
#endif
#if (ECAT_FOE_HEADER_BUF_MAX) & (1UL << 4)
#define ECAT_FOE_ABI_H4 1
#else
#define ECAT_FOE_ABI_H4 0
#endif
#if (ECAT_FOE_HEADER_BUF_MAX) & (1UL << 3)
#define ECAT_FOE_ABI_H3 1
#else
#define ECAT_FOE_ABI_H3 0
#endif
#if (ECAT_FOE_HEADER_BUF_MAX) & (1UL << 2)
#define ECAT_FOE_ABI_H2 1
#else
#define ECAT_FOE_ABI_H2 0
#endif
#if (ECAT_FOE_HEADER_BUF_MAX) & (1UL << 1)
#define ECAT_FOE_ABI_H1 1
#else
#define ECAT_FOE_ABI_H1 0
#endif
#if (ECAT_FOE_HEADER_BUF_MAX) & (1UL << 0)
#define ECAT_FOE_ABI_H0 1
#else
#define ECAT_FOE_ABI_H0 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 31)
#define ECAT_FOE_ABI_W31 1
#else
#define ECAT_FOE_ABI_W31 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 30)
#define ECAT_FOE_ABI_W30 1
#else
#define ECAT_FOE_ABI_W30 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 29)
#define ECAT_FOE_ABI_W29 1
#else
#define ECAT_FOE_ABI_W29 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 28)
#define ECAT_FOE_ABI_W28 1
#else
#define ECAT_FOE_ABI_W28 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 27)
#define ECAT_FOE_ABI_W27 1
#else
#define ECAT_FOE_ABI_W27 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 26)
#define ECAT_FOE_ABI_W26 1
#else
#define ECAT_FOE_ABI_W26 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 25)
#define ECAT_FOE_ABI_W25 1
#else
#define ECAT_FOE_ABI_W25 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 24)
#define ECAT_FOE_ABI_W24 1
#else
#define ECAT_FOE_ABI_W24 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 23)
#define ECAT_FOE_ABI_W23 1
#else
#define ECAT_FOE_ABI_W23 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 22)
#define ECAT_FOE_ABI_W22 1
#else
#define ECAT_FOE_ABI_W22 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 21)
#define ECAT_FOE_ABI_W21 1
#else
#define ECAT_FOE_ABI_W21 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 20)
#define ECAT_FOE_ABI_W20 1
#else
#define ECAT_FOE_ABI_W20 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 19)
#define ECAT_FOE_ABI_W19 1
#else
#define ECAT_FOE_ABI_W19 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 18)
#define ECAT_FOE_ABI_W18 1
#else
#define ECAT_FOE_ABI_W18 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 17)
#define ECAT_FOE_ABI_W17 1
#else
#define ECAT_FOE_ABI_W17 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 16)
#define ECAT_FOE_ABI_W16 1
#else
#define ECAT_FOE_ABI_W16 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 15)
#define ECAT_FOE_ABI_W15 1
#else
#define ECAT_FOE_ABI_W15 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 14)
#define ECAT_FOE_ABI_W14 1
#else
#define ECAT_FOE_ABI_W14 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 13)
#define ECAT_FOE_ABI_W13 1
#else
#define ECAT_FOE_ABI_W13 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 12)
#define ECAT_FOE_ABI_W12 1
#else
#define ECAT_FOE_ABI_W12 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 11)
#define ECAT_FOE_ABI_W11 1
#else
#define ECAT_FOE_ABI_W11 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 10)
#define ECAT_FOE_ABI_W10 1
#else
#define ECAT_FOE_ABI_W10 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 9)
#define ECAT_FOE_ABI_W9 1
#else
#define ECAT_FOE_ABI_W9 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 8)
#define ECAT_FOE_ABI_W8 1
#else
#define ECAT_FOE_ABI_W8 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 7)
#define ECAT_FOE_ABI_W7 1
#else
#define ECAT_FOE_ABI_W7 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 6)
#define ECAT_FOE_ABI_W6 1
#else
#define ECAT_FOE_ABI_W6 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 5)
#define ECAT_FOE_ABI_W5 1
#else
#define ECAT_FOE_ABI_W5 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 4)
#define ECAT_FOE_ABI_W4 1
#else
#define ECAT_FOE_ABI_W4 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 3)
#define ECAT_FOE_ABI_W3 1
#else
#define ECAT_FOE_ABI_W3 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 2)
#define ECAT_FOE_ABI_W2 1
#else
#define ECAT_FOE_ABI_W2 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 1)
#define ECAT_FOE_ABI_W1 1
#else
#define ECAT_FOE_ABI_W1 0
#endif
#if (ECAT_FOE_MAX_WRITE_UNIT) & (1UL << 0)
#define ECAT_FOE_ABI_W0 1
#else
#define ECAT_FOE_ABI_W0 0
#endif

#define ECAT_FOE_ABI_JOIN_IMPL(h15, h14, h13, h12, h11, h10, h9, h8, h7, h6, h5, h4, h3, h2, h1, h0, w31, w30, w29, w28, w27, w26, w25, w24, w23, w22, w21, w20, w19, w18, w17, w16, w15, w14, w13, w12, w11, w10, w9, w8, w7, w6, w5, w4, w3, w2, w1, w0) \
    ecat_foe_config_abi_v2_h##h15##h14##h13##h12##h11##h10##h9##h8##h7##h6##h5##h4##h3##h2##h1##h0##_w##w31##w30##w29##w28##w27##w26##w25##w24##w23##w22##w21##w20##w19##w18##w17##w16##w15##w14##w13##w12##w11##w10##w9##w8##w7##w6##w5##w4##w3##w2##w1##w0
#define ECAT_FOE_ABI_JOIN(h15, h14, h13, h12, h11, h10, h9, h8, h7, h6, h5, h4, h3, h2, h1, h0, w31, w30, w29, w28, w27, w26, w25, w24, w23, w22, w21, w20, w19, w18, w17, w16, w15, w14, w13, w12, w11, w10, w9, w8, w7, w6, w5, w4, w3, w2, w1, w0) \
    ECAT_FOE_ABI_JOIN_IMPL(h15, h14, h13, h12, h11, h10, h9, h8, h7, h6, h5, h4, h3, h2, h1, h0, w31, w30, w29, w28, w27, w26, w25, w24, w23, w22, w21, w20, w19, w18, w17, w16, w15, w14, w13, w12, w11, w10, w9, w8, w7, w6, w5, w4, w3, w2, w1, w0)
#define ECAT_FOE_ABI_SYMBOL ECAT_FOE_ABI_JOIN(ECAT_FOE_ABI_H15, ECAT_FOE_ABI_H14, ECAT_FOE_ABI_H13, ECAT_FOE_ABI_H12, ECAT_FOE_ABI_H11, ECAT_FOE_ABI_H10, ECAT_FOE_ABI_H9, ECAT_FOE_ABI_H8, ECAT_FOE_ABI_H7, ECAT_FOE_ABI_H6, ECAT_FOE_ABI_H5, ECAT_FOE_ABI_H4, ECAT_FOE_ABI_H3, ECAT_FOE_ABI_H2, ECAT_FOE_ABI_H1, ECAT_FOE_ABI_H0, ECAT_FOE_ABI_W31, ECAT_FOE_ABI_W30, ECAT_FOE_ABI_W29, ECAT_FOE_ABI_W28, ECAT_FOE_ABI_W27, ECAT_FOE_ABI_W26, ECAT_FOE_ABI_W25, ECAT_FOE_ABI_W24, ECAT_FOE_ABI_W23, ECAT_FOE_ABI_W22, ECAT_FOE_ABI_W21, ECAT_FOE_ABI_W20, ECAT_FOE_ABI_W19, ECAT_FOE_ABI_W18, ECAT_FOE_ABI_W17, ECAT_FOE_ABI_W16, ECAT_FOE_ABI_W15, ECAT_FOE_ABI_W14, ECAT_FOE_ABI_W13, ECAT_FOE_ABI_W12, ECAT_FOE_ABI_W11, ECAT_FOE_ABI_W10, ECAT_FOE_ABI_W9, ECAT_FOE_ABI_W8, ECAT_FOE_ABI_W7, ECAT_FOE_ABI_W6, ECAT_FOE_ABI_W5, ECAT_FOE_ABI_W4, ECAT_FOE_ABI_W3, ECAT_FOE_ABI_W2, ECAT_FOE_ABI_W1, ECAT_FOE_ABI_W0)

#endif
