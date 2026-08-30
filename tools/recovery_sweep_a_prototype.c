/* Prototype for shape (A) of the §4 delegation test: ONE call sweeps the
 * whole speed ladder internally and returns a Q-CRC-union stream plus
 * per-rung PCM, instead of the caller looping accudisc_read_cdda itself
 * (shape B). NOT public API — a standalone harness against the existing
 * library, built to compare against cdda2img's (B) arm on the same span.
 *
 * Span is the one agreed with cdda2img: LBA [110000, 118000) on Tracy
 * Chapman / PX-716A. Ladder order is an argument, not fixed — added so the
 * SAME binary can run the original [40,32,24,8,4] arm and the reversed-order
 * confound check [4,8,24,32,40] without a second source file. The original
 * order stays the default so the earlier run stays exactly reproducible.
 *
 * Build: cc -O2 -Iinclude -o /tmp/rsweep_a tools/recovery_sweep_a_prototype.c \
 *          -Lbuild/src -laccudisc -Wl,-rpath,build/src
 * Run:   /tmp/rsweep_a /dev/sr0 [speed,speed,...] > /var/tmp/rsweep_a.json
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <accudisc/accudisc.h>

#define SPAN_LBA   110000u
#define SPAN_COUNT 8000u
#define NRUNGS     5
static uint16_t SPEEDS[NRUNGS] = {40, 32, 24, 8, 4};

/* Parses "4,8,24,32,40" into SPEEDS, in order, exactly NRUNGS entries. */
static void parse_speeds(const char *csv)
{
    char buf[64];
    strncpy(buf, csv, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    int n = 0;
    for (char *tok = strtok(buf, ","); tok && n < NRUNGS; tok = strtok(NULL, ","))
        SPEEDS[n++] = (uint16_t)atoi(tok);
    if (n != NRUNGS) {
        fprintf(stderr, "speed list must have exactly %d entries, got %d\n", NRUNGS, n);
        exit(1);
    }
}

#define AUDIO_LEN 2352u /* 588 samples * 4 bytes */
#define SUB_LEN   96u

typedef struct {
    uint8_t *audio;   /* SPAN_COUNT * AUDIO_LEN */
    uint8_t *sub;      /* SPAN_COUNT * SUB_LEN, raw P-W */
    uint16_t requested_x, honoured_x;
    double delivered_x;
    double elapsed_s;
    accudisc_read_stats stats;
} rung_t;

typedef struct {
    uint32_t base_lba;
    uint8_t *audio; /* filled as chunks arrive */
    uint8_t *sub;
} sink_ctx_t;

static int sink_fn(void *user, const accudisc_chunk *c)
{
    sink_ctx_t *ctx = (sink_ctx_t *)user;
    uint32_t off = c->lba - ctx->base_lba;
    for (uint32_t i = 0; i < c->nsec; i++) {
        const uint8_t *sec = c->data + (size_t)i * c->sector_len;
        memcpy(ctx->audio + (size_t)(off + i) * AUDIO_LEN, sec, AUDIO_LEN);
        if (c->sub_len == SUB_LEN)
            memcpy(ctx->sub + (size_t)(off + i) * SUB_LEN,
                   sec + c->audio_len + c->c2_len, SUB_LEN);
    }
    return 0;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/sr0";
    if (argc > 2)
        parse_speeds(argv[2]);
    int err = 0;
    double t_call_start = now_s();

    accudisc_device *dev = accudisc_open(path, 0, &err);
    if (!dev) {
        fprintf(stderr, "accudisc_open(%s) failed: err=%d\n", path, err);
        return 1;
    }

    rung_t rungs[NRUNGS];
    memset(rungs, 0, sizeof(rungs));

    for (int r = 0; r < NRUNGS; r++) {
        rungs[r].audio = calloc(SPAN_COUNT, AUDIO_LEN);
        rungs[r].sub = calloc(SPAN_COUNT, SUB_LEN);
        if (!rungs[r].audio || !rungs[r].sub) {
            fprintf(stderr, "OOM allocating rung %d buffers\n", r);
            return 1;
        }

        sink_ctx_t ctx = { .base_lba = SPAN_LBA, .audio = rungs[r].audio,
                            .sub = rungs[r].sub };

        accudisc_read_req req;
        memset(&req, 0, sizeof(req));
        req.size = sizeof(req);
        req.lba = SPAN_LBA;
        req.count = SPAN_COUNT;
        req.c2 = ACCUDISC_C2_NONE;
        req.sub = ACCUDISC_SUB_RAW;
        req.speed_x = SPEEDS[r];

        accudisc_read_stats stats;
        memset(&stats, 0, sizeof(stats));
        stats.size = sizeof(stats);

        double t0 = now_s();
        int rc = accudisc_read_cdda(dev, &req, sink_fn, &ctx, &stats);
        double t1 = now_s();

        if (rc != ACCUDISC_OK) {
            fprintf(stderr, "rung %d (speed_x=%u): accudisc_read_cdda failed rc=%d\n",
                    r, SPEEDS[r], rc);
            accudisc_close(dev);
            return 1;
        }

        rungs[r].requested_x = stats.speed_requested_x;
        rungs[r].honoured_x = stats.speed_honoured_x;
        rungs[r].elapsed_s = t1 - t0;
        rungs[r].delivered_x = (rungs[r].elapsed_s > 0)
            ? ((double)SPAN_COUNT / rungs[r].elapsed_s) / 75.0 : 0.0;
        rungs[r].stats = stats;
    }

    accudisc_close(dev);
    double t_call_end = now_s();

    /* Duplicate-honoured-speed detection across rungs actually read. */
    int dup_pairs = 0;
    for (int i = 0; i < NRUNGS; i++)
        for (int j = i + 1; j < NRUNGS; j++)
            if (rungs[i].honoured_x != 0 && rungs[i].honoured_x == rungs[j].honoured_x)
                dup_pairs++;

    /* Per-frame Q CRC-16 union: first CRC-valid rung wins, in rung order
     * (index 0 = fastest requested, per Keith's ruling "first pass with a
     * valid CRC"). Provenance recorded per frame. */
    int8_t *provenance = malloc(SPAN_COUNT); /* -1 = unrecovered */
    uint32_t recovered = 0, unrecovered = 0;
    for (uint32_t i = 0; i < SPAN_COUNT; i++) {
        provenance[i] = -1;
        for (int r = 0; r < NRUNGS; r++) {
            uint8_t q12[12];
            accudisc_sub_extract_q(rungs[r].sub + (size_t)i * SUB_LEN, q12);
            accudisc_q q;
            if (accudisc_q_parse(q12, &q) == ACCUDISC_OK && q.crc_ok) {
                provenance[i] = (int8_t)r;
                break;
            }
        }
        if (provenance[i] >= 0)
            recovered++;
        else
            unrecovered++;
    }

    /* Provenance histogram: which rung ended up supplying the merged frame,
     * for frames that had a choice among more than one CRC-valid rung vs
     * frames only one rung ever got right. */
    uint32_t supplied_by[NRUNGS] = {0};
    for (uint32_t i = 0; i < SPAN_COUNT; i++)
        if (provenance[i] >= 0)
            supplied_by[(int)provenance[i]]++;

    /* ---- JSON report -------------------------------------------------- */
    printf("{\n");
    printf("  \"shape\": \"A\",\n");
    printf("  \"ladder_order\": [%u,%u,%u,%u,%u],\n",
           SPEEDS[0], SPEEDS[1], SPEEDS[2], SPEEDS[3], SPEEDS[4]);
    printf("  \"span_lba_start\": %u,\n  \"span_count\": %u,\n", SPAN_LBA, SPAN_COUNT);
    printf("  \"call_wall_clock_s\": %.3f,\n", t_call_end - t_call_start);
    printf("  \"rungs\": [\n");
    for (int r = 0; r < NRUNGS; r++) {
        printf("    {\"idx\": %d, \"requested_x\": %u, \"honoured_x\": %u, "
               "\"delivered_x\": %.2f, \"elapsed_s\": %.3f, "
               "\"sectors_flagged\": %llu, \"hard_errors\": %llu, "
               "\"subq_ok\": %llu, \"subq_total\": %llu, "
               "\"q_frames_supplied\": %u}%s\n",
               r, rungs[r].requested_x, rungs[r].honoured_x, rungs[r].delivered_x,
               rungs[r].elapsed_s,
               (unsigned long long)rungs[r].stats.sectors_flagged,
               (unsigned long long)rungs[r].stats.hard_errors,
               (unsigned long long)rungs[r].stats.subq_ok,
               (unsigned long long)rungs[r].stats.subq_total,
               supplied_by[r],
               r + 1 < NRUNGS ? "," : "");
    }
    printf("  ],\n");
    printf("  \"duplicate_honoured_speed_pairs\": %d,\n", dup_pairs);
    printf("  \"q_union\": {\"recovered\": %u, \"unrecovered\": %u, \"total\": %u}\n",
           recovered, unrecovered, SPAN_COUNT);
    printf("}\n");

    for (int r = 0; r < NRUNGS; r++) {
        free(rungs[r].audio);
        free(rungs[r].sub);
    }
    free(provenance);
    return 0;
}
