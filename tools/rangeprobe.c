/* Phase 3: is a ranged SET STREAMING contract LOCAL (throttle only inside
 * [L,L+N), free-run outside) or GLOBAL on the PX-716A? One run, several
 * contracts, each timed inside vs outside the nominal slow zone and classified
 * against the free-run baseline. Needs CAP_SYS_RAWIO (SET STREAMING data-OUT);
 * timed reads are data-IN. Restores full speed. */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <accudisc/accudisc.h>

#include "internal.h"
#include "mmc/mmc.h"

static const uint32_t IN = 100000, OUT = 150000, N = 4000, M = 3000;

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void desc_at(uint8_t *d, unsigned speed_x, uint32_t start, uint32_t end)
{
    uint32_t rs = speed_x * 1764u / 10u;
    memset(d, 0, 28);
    put_be32(d + 4, start); put_be32(d + 8, end);
    put_be32(d + 12, rs); put_be32(d + 16, 1000);
    put_be32(d + 20, rs); put_be32(d + 24, 1000);
}
static int set_streaming_n(accudisc_device *dev, const uint8_t *desc, unsigned n)
{
    adsc_cmd c;
    uint16_t plen = (uint16_t)(n * 28);
    memset(&c, 0, sizeof(c));
    c.cdb[0] = 0xB6;
    c.cdb[9] = (uint8_t)(plen >> 8); c.cdb[10] = (uint8_t)(plen & 0xff);
    c.cdb_len = 12; c.dir = ADSC_XFER_OUT;
    c.buf = (void *)desc; c.buf_len = plen; c.timeout_ms = 15000;
    return adsc_dev_exec(dev, &c);
}
/* Chunked timed read (single READ CD is capped by the SG reserved buffer). */
static double read_time(accudisc_device *dev, uint32_t lba, uint32_t nsec)
{
    static uint8_t buf[32 * 2352];
    const uint32_t CHUNK = 25;
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (uint32_t off = 0; off < nsec; off += CHUNK) {
        uint32_t k = nsec - off < CHUNK ? nsec - off : CHUNK;
        if (adsc_mmc_read_cd(dev, lba + off, k, ADSC_SECTOR_CDDA,
                             ADSC_C2_NONE, ADSC_SUB_NONE, buf, 2352)
            != ACCUDISC_OK)
            return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

static double base_in, base_out;

static const char *cls(double t, double base)
{
    if (t <= 0) return "FAIL";
    return t > base * 2.0 ? "SLOW" : "fast";
}

/* Set contract, read page 2A, time IN and OUT, classify vs baseline. */
static void run(accudisc_device *dev, const char *name, const uint8_t *d,
                unsigned n)
{
    int rc = set_streaming_n(dev, d, n);
    accudisc_sense s; accudisc_last_sense(dev, &s);
    unsigned mx = 0, cx = 0; accudisc_get_speed(dev, &mx, &cx);
    printf("%-28s rc=%d page2A=%.0fx", name, rc, cx / 176.4);
    if (rc != ACCUDISC_OK && s.valid)
        printf(" key=0x%x/%02x/%02x", s.key, s.asc, s.ascq);
    putchar('\n');
    if (rc != ACCUDISC_OK) return;
    read_time(dev, OUT, 200); /* warm */
    double tin = read_time(dev, IN, M), tout = read_time(dev, OUT, M);
    printf("    IN %.2fs [%s]   OUT %.2fs [%s]\n",
           tin, cls(tin, base_in), tout, cls(tout, base_out));
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/sr0";
    int err = 0;
    accudisc_device *dev = accudisc_open(path, ACCUDISC_OPEN_RDWR, &err);
    if (!dev) { fprintf(stderr, "open: %s\n", accudisc_strerror(err)); return 1; }

    accudisc_set_speed(dev, 40);
    read_time(dev, IN, 200); read_time(dev, OUT, 200); /* warm both */
    base_in = read_time(dev, IN, M);
    base_out = read_time(dev, OUT, M);
    printf("baseline free-run: IN %.2fs  OUT %.2fs  (SLOW = >2x baseline)\n\n",
           base_in, base_out);

    uint8_t d[3 * 28];

    /* Positive control: 4x whole disc. Both must go SLOW or the rig is broken. */
    desc_at(d, 4, 0, 0xFFFFFFFFu);
    run(dev, "A. 4x whole-disc (control)", d, 1);
    accudisc_set_speed(dev, 40);

    /* Single descriptor, 4x ranged to the IN span only. */
    desc_at(d, 4, IN, IN + N);
    run(dev, "B. 4x single-desc [IN)", d, 1);
    accudisc_set_speed(dev, 40);

    /* 3 descriptors, slow in the MIDDLE (40 / 4 / 40). */
    desc_at(d + 0, 40, 0, IN);
    desc_at(d + 28, 4, IN, IN + N);
    desc_at(d + 56, 40, IN + N, 0xFFFFFFFFu);
    run(dev, "C. 3-desc middle-slow", d, 3);
    accudisc_set_speed(dev, 40);

    /* 3 descriptors, slow FIRST (4 / 40 / 40) — the confirming case. */
    desc_at(d + 0, 4, 0, IN);
    desc_at(d + 28, 40, IN, IN + N);
    desc_at(d + 56, 40, IN + N, 0xFFFFFFFFu);
    run(dev, "D. 3-desc first-slow", d, 3);
    accudisc_set_speed(dev, 40);

    accudisc_close(dev);
    return 0;
}
