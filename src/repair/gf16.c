/* SPDX-License-Identifier: MIT */
/* GF(2^16) arithmetic — see gf16.h for the field constants and why they are
 * proved rather than assumed. */

#include "gf16.h"

/* x^16 + x^12 + x^3 + x + 1. The x^16 term is the bit that falls off the
 * top of a uint16_t, so the reduction below tests bit 16 of a widened
 * accumulator and folds in the low 16 bits of this constant. */
#define GF16_POLY 0x1100Bu

/* 393210 bytes of tables. Doubled antilog: an exponent sum reaches
 * 2*65534 = 131068, so no index needs reducing before the lookup. */
uint16_t adsc_gf16_exp_tab[2 * ADSC_GF16_ORDER];
uint16_t adsc_gf16_log_tab[65536];
static int gf_ready;

void adsc_gf16_init(void)
{
    unsigned x = 1;

    if (gf_ready)
        return;
    for (unsigned i = 0; i < ADSC_GF16_ORDER; i++) {
        adsc_gf16_exp_tab[i] = (uint16_t)x;
        adsc_gf16_exp_tab[i + ADSC_GF16_ORDER] = (uint16_t)x;
        adsc_gf16_log_tab[x] = (uint16_t)i;
        x <<= 1; /* multiply by alpha = 2 */
        if (x & 0x10000u)
            x ^= GF16_POLY;
    }
    /* adsc_gf16_log_tab[0] is never consulted: every path below excludes zero
     * first, and adsc_gf16_log() answers for it out of band. */
    adsc_gf16_log_tab[0] = 0;
    gf_ready = 1;
}

uint16_t adsc_gf16_mul(uint16_t a, uint16_t b)
{
    adsc_gf16_init();
    if (!a || !b)
        return 0;
    return adsc_gf16_exp_tab[(unsigned)adsc_gf16_log_tab[a] + adsc_gf16_log_tab[b]];
}

uint16_t adsc_gf16_div(uint16_t a, uint16_t b)
{
    adsc_gf16_init();
    if (!a || !b)
        return 0;
    return adsc_gf16_exp_tab[(unsigned)adsc_gf16_log_tab[a] + ADSC_GF16_ORDER - adsc_gf16_log_tab[b]];
}

uint16_t adsc_gf16_inv(uint16_t a)
{
    adsc_gf16_init();
    if (!a)
        return 0;
    return adsc_gf16_exp_tab[ADSC_GF16_ORDER - adsc_gf16_log_tab[a]];
}

uint16_t adsc_gf16_pow(unsigned e)
{
    adsc_gf16_init();
    return adsc_gf16_exp_tab[e % ADSC_GF16_ORDER];
}

uint16_t adsc_gf16_mul_pow(uint16_t a, unsigned e)
{
    adsc_gf16_init();
    if (!a)
        return 0;
    return adsc_gf16_exp_tab[(unsigned)adsc_gf16_log_tab[a] + e % ADSC_GF16_ORDER];
}

int adsc_gf16_log(uint16_t a)
{
    adsc_gf16_init();
    if (!a)
        return ADSC_GF16_LOG_UNDEFINED;
    return adsc_gf16_log_tab[a];
}
