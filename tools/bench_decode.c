/* Isolated decode-path microbenchmark — no device, pure CPU.
 *
 * Answers the question the 2026-07-23 optimisation audit left open: do the
 * per-sector decode leaves cost enough CPU to be worth optimising, and how much
 * would the proposed alternatives actually save? A rip is drive-bound, so this
 * measures *headroom*, not throughput — the summary puts the numbers next to the
 * ~120 s wall-clock of a 360k-sector rip at 40x so "worth it?" is answered in
 * context.
 *
 * FAIRNESS: every variant (current AND proposed) is a local copy compiled here
 * at one -O level, so the comparison is apples-to-apples regardless of how
 * libaccudisc.a was built (a default CMake build has no -O). The linked library
 * functions are used ONLY to validate the local copies are byte-faithful, never
 * timed.
 *
 * ANTI-HOIST: the leaves are pure and would be loop-invariant if called on one
 * fixed buffer — the optimiser then computes them ONCE and reports a fictional
 * speedup. So each stream is an array of NB distinct sectors indexed by the loop
 * counter (s & (NB-1)); the accessed buffer varies every iteration, which the
 * compiler cannot hoist, and results accumulate into a volatile sink to defeat
 * dead-code elimination.
 *
 * Build (needs -O2 for meaningful numbers; no device, no CAP_SYS_RAWIO):
 *   cmake --build build
 *   gcc -O2 -o build/bench_decode tools/bench_decode.c \
 *       -I include -I src build/src/libaccudisc.a
 *   ./build/bench_decode
 *
 * Caveat: NB sectors (~80 KB) stay warm in L1/L2, so this isolates each
 * function's compute cost. Real sectors arrive cold from I/O, but the drive
 * delivers them slowly enough that the compute is what we are isolating.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <accudisc/accudisc.h> /* ACCUDISC_BYTES_*, accudisc_sub_extract_q */

#include "cdda/crc16.h"        /* adsc_crc16 (validation oracle) */
#include "read/engine.h"       /* adsc_audio_diff (validation oracle) */

#define SECTORS 360000u        /* ~ a full 74-min disc */
#define REPS    5              /* take the min, to shed scheduler noise */
#define NB      16             /* distinct sectors cycled through (anti-hoist) */
#define AUDIO   ACCUDISC_BYTES_AUDIO   /* 2352 */
#define C2LEN   ACCUDISC_BYTES_C2      /* 294  */
#define QLEN    10             /* Q CRC covers 10 bytes */

/* ---- variants under test (local copies, one -O level) --------------------- */

/* #1 audio_diff: pre-optimisation scalar vs current memcmp fast-path. */
static uint32_t audio_diff_scalar(const uint8_t *a, const uint8_t *b)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < AUDIO; i++)
        n += a[i] != b[i];
    return n;
}
static uint32_t audio_diff_memcmp(const uint8_t *a, const uint8_t *b)
{
    if (memcmp(a, b, AUDIO) == 0)
        return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < AUDIO; i++)
        n += a[i] != b[i];
    return n;
}

/* #2 popcount: current byte-at-a-time vs proposed word-at-a-time. */
static uint32_t popcount_byte(const uint8_t *p, uint32_t n)
{
    uint32_t bits = 0;
    for (uint32_t i = 0; i < n; i++)
        bits += (uint32_t)__builtin_popcount(p[i]);
    return bits;
}
static uint32_t popcount_word(const uint8_t *p, uint32_t n)
{
    uint32_t bits = 0, i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        bits += (uint32_t)__builtin_popcountll(w);
    }
    for (; i < n; i++)
        bits += (uint32_t)__builtin_popcount(p[i]);
    return bits;
}

/* #3 CRC-16/X.25 (poly 0x1021, init 0, non-reflected): bitwise vs table. */
static uint16_t crc16_bitwise(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1));
    }
    return crc;
}
static uint16_t crc16_tab[256];
static void crc16_table_init(void)
{
    for (int n = 0; n < 256; n++) {
        uint16_t c = (uint16_t)(n << 8);
        for (int k = 0; k < 8; k++)
            c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x1021 : (c << 1));
        crc16_tab[n] = c;
    }
}
static uint16_t crc16_table(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++)
        crc = (uint16_t)((crc << 8) ^ crc16_tab[(crc >> 8) ^ data[i]]);
    return crc;
}

/* extract_q: current bit-transpose (LOW; no portable alternative, timed only to
 * gauge whether it is hot enough to bother with a pext path). */
static void extract_q_current(const uint8_t raw[96], uint8_t q[12])
{
    memset(q, 0, 12);
    for (unsigned i = 0; i < 96; i++)
        q[i >> 3] = (uint8_t)((q[i >> 3] << 1) | ((raw[i] >> 6) & 1));
}

/* ---- timing --------------------------------------------------------------- */

static volatile uint64_t g_sink; /* defeats dead-code elimination */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double ms(uint64_t ns) { return (double)ns / 1e6; }

static void row(const char *name, uint64_t ns, double *store)
{
    printf("    %-26s %8.2f ms/disc  %7.2f ns/call\n",
           name, ms(ns), (double)ns / SECTORS);
    if (store)
        *store = ms(ns);
}

/* NB distinct sectors per stream; [s & (NB-1)] cycles them so no call is
 * loop-invariant. */
static uint8_t audio_a[NB][AUDIO], audio_b[NB][AUDIO];
static uint8_t c2[NB][C2LEN], sub[NB][96];

int main(void)
{
    srand(1);
    for (unsigned k = 0; k < NB; k++) {
        for (uint32_t i = 0; i < AUDIO; i++)
            audio_a[k][i] = (uint8_t)rand();
        memcpy(audio_b[k], audio_a[k], AUDIO); /* equal case to start */
        for (uint32_t i = 0; i < C2LEN; i++)
            c2[k][i] = (uint8_t)rand();
        for (unsigned i = 0; i < 96; i++)
            sub[k][i] = (uint8_t)rand();
    }
    crc16_table_init();

    /* -- validate the local copies against the library / each other before
     *    trusting any timing (a fast wrong answer is worthless). -- */
    {
        uint8_t diff_b[AUDIO];
        memcpy(diff_b, audio_a[0], AUDIO);
        diff_b[AUDIO - 1] ^= 0xff; /* differ at the worst spot for memcmp */
        if (audio_diff_memcmp(audio_a[0], diff_b) != audio_diff_scalar(audio_a[0], diff_b) ||
            audio_diff_memcmp(audio_a[0], diff_b) != adsc_audio_diff(audio_a[0], diff_b))
            return fprintf(stderr, "FAIL: audio_diff variants disagree\n"), 1;
        if (popcount_word(c2[0], C2LEN) != popcount_byte(c2[0], C2LEN))
            return fprintf(stderr, "FAIL: popcount variants disagree\n"), 1;
        if (crc16_table(sub[0], QLEN) != crc16_bitwise(sub[0], QLEN) ||
            crc16_table(sub[0], QLEN) != adsc_crc16(sub[0], QLEN))
            return fprintf(stderr, "FAIL: crc16 variants disagree\n"), 1;
        uint8_t qa[12], qb[12];
        extract_q_current(sub[0], qa);
        accudisc_sub_extract_q(sub[0], qb);
        if (memcmp(qa, qb, 12) != 0)
            return fprintf(stderr, "FAIL: extract_q disagrees with library\n"), 1;
    }

    printf("AccuDisc decode-path microbenchmark — CPU-isolated, no device\n");
    printf("  %u sectors/disc, min of %d reps, %d-sector working set, CLOCK_MONOTONIC\n\n",
           SECTORS, REPS, NB);

    double t_ad_eq = 0, t_ad_eq_old = 0, t_ad_df = 0, t_ad_df_old = 0;
    double t_pc_cur = 0, t_pc_new = 0, t_crc_cur = 0, t_crc_new = 0, t_xq = 0;

    /* Macro: min-of-REPS timed loop over SECTORS, body uses index k = s&(NB-1). */
#define BENCH(best, BODY) do {                                           \
        best = UINT64_MAX;                                              \
        for (int rep = 0; rep < REPS; rep++) {                         \
            uint64_t acc = 0, t0 = now_ns();                           \
            for (uint32_t s = 0; s < SECTORS; s++) {                   \
                unsigned k = s & (NB - 1); (void)k; BODY               \
            }                                                          \
            uint64_t dt = now_ns() - t0;                               \
            g_sink += acc;                                             \
            if (dt < best) best = dt;                                  \
        }                                                              \
    } while (0)

    uint64_t best;

    printf("[#1] adsc_audio_diff  (2352 B, per verify/overlap sector)\n");
    BENCH(best, acc += audio_diff_memcmp(audio_a[k], audio_b[k]););
    row("equal  memcmp (current)", best, &t_ad_eq);
    BENCH(best, acc += audio_diff_scalar(audio_a[k], audio_b[k]););
    row("equal  scalar (old)", best, &t_ad_eq_old);
    /* differ at the last byte = worst case for the memcmp fast-path. */
    for (unsigned k = 0; k < NB; k++)
        audio_b[k][AUDIO - 1] ^= 0xff;
    BENCH(best, acc += audio_diff_memcmp(audio_a[k], audio_b[k]););
    row("differ memcmp (current)", best, &t_ad_df);
    BENCH(best, acc += audio_diff_scalar(audio_a[k], audio_b[k]););
    row("differ scalar (old)", best, &t_ad_df_old);
    for (unsigned k = 0; k < NB; k++)
        audio_b[k][AUDIO - 1] ^= 0xff; /* restore equal */
    printf("      -> equal-case %.1fx, differ-case %.2fx\n\n",
           t_ad_eq_old / t_ad_eq, t_ad_df_old / t_ad_df);

    printf("[#2] popcount C2 bitmap  (294 B, per sector)\n");
    BENCH(best, acc += popcount_byte(c2[k], C2LEN););
    row("byte-at-a-time (current)", best, &t_pc_cur);
    BENCH(best, acc += popcount_word(c2[k], C2LEN););
    row("word-at-a-time (proposed)", best, &t_pc_new);
    printf("      -> %.1fx\n\n", t_pc_cur / t_pc_new);

    printf("[#3] CRC-16  (10 B Q frame, per SUB_RAW sector)\n");
    BENCH(best, acc += crc16_bitwise(sub[k], QLEN););
    row("bitwise (current)", best, &t_crc_cur);
    BENCH(best, acc += crc16_table(sub[k], QLEN););
    row("table (proposed)", best, &t_crc_new);
    printf("      -> %.1fx\n\n", t_crc_cur / t_crc_new);

    printf("[--] extract_q bit-transpose  (96 B, per SUB_RAW sector)  [LOW]\n");
    BENCH(best, uint8_t q[12]; extract_q_current(sub[k], q); acc += q[0];);
    row("current (portable)", best, &t_xq);
    printf("\n");

    /* -- context: sum the CURRENT per-sector decode path against the rip wall
     *    clock. A --pcm+C2+SUB rip with one verify pass runs, per sector:
     *    audio_diff (equal) + popcount + crc16 + extract_q. -- */
    double decode_ms = t_ad_eq + t_pc_cur + t_crc_cur + t_xq;
    double rip_s = (double)SECTORS / 3000.0; /* 40x ~ 3000 sectors/s */
    printf("Context — current per-sector decode CPU vs a 40x rip:\n");
    printf("    decode total : %.1f ms/disc  (audio_diff+popcount+crc16+extract_q)\n",
           decode_ms);
    printf("    rip wall     : %.0f s/disc  (%.0f sectors at ~3000/s)\n",
           rip_s, (double)SECTORS);
    printf("    decode share : %.3f%% of the rip\n", decode_ms / (rip_s * 1000.0) * 100.0);
    printf("    with #2+#3   : %.1f ms/disc  (saves %.1f ms/disc)\n",
           t_ad_eq + t_pc_new + t_crc_new + t_xq,
           (t_pc_cur - t_pc_new) + (t_crc_cur - t_crc_new));

    printf("\n(sink %llu)\n", (unsigned long long)g_sink);
    return 0;
}
