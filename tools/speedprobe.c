/* Speed-control probe, clean ordering: no SET CD SPEED to muddy drive state.
 * Answers: (1) does GET PERFORMANCE reflect a set ceiling, or is it a static
 * nominal? (2) does the Exact bit (0x02) work -> CLV? (3) does real RDD (0x04)
 * restore, unlike our current wrong 0x20? */

#include <stdio.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "internal.h"
#include "mmc/mmc.h"
#include "transport/transport.h"

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void sense_of(accudisc_device *dev, int rc, const char *what)
{
    accudisc_sense s;
    accudisc_last_sense(dev, &s);
    printf("  %-34s rc=%d", what, rc);
    if (rc == ACCUDISC_OK)
        printf("  OK");
    else if (s.valid)
        printf("  key=0x%x asc=0x%02x ascq=0x%02x", s.key, s.asc, s.ascq);
    putchar('\n');
}

static void get_perf(accudisc_device *dev, const char *tag)
{
    uint8_t pbuf[8 + 16 * 8];
    adsc_cmd c;
    memset(&c, 0, sizeof(c));
    memset(pbuf, 0, sizeof(pbuf));
    c.cdb[0] = 0xAC; c.cdb[9] = 0x08; c.cdb[10] = 0x00;
    c.cdb_len = 12; c.dir = ADSC_XFER_IN;
    c.buf = pbuf; c.buf_len = sizeof(pbuf); c.timeout_ms = 10000;
    int rc = adsc_dev_exec(dev, &c);
    if (rc != ACCUDISC_OK) {
        sense_of(dev, rc, tag);
        return;
    }
    uint32_t dlen = be32(pbuf);
    uint32_t n = dlen >= 4 ? (dlen - 4) / 16 : 0;
    printf("  %-34s %u desc:", tag, n);
    if (n > 8) n = 8;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *d = pbuf + 8 + i * 16;
        printf("  [lba %u..%u %.2fx..%.2fx]", be32(d), be32(d + 8),
               be32(d + 4) / 176.4, be32(d + 12) / 176.4);
    }
    putchar('\n');
}

/* SET STREAMING with explicit flags so we can test bit meanings. */
static int set_streaming(accudisc_device *dev, uint8_t flags, unsigned speed_x,
                         uint32_t start, uint32_t end)
{
    uint8_t d[28];
    adsc_cmd c;
    memset(d, 0, sizeof(d));
    uint32_t rs = speed_x * 1764u / 10u;
    d[0] = flags;
    put_be32(d + 4, start);
    put_be32(d + 8, end);
    if (speed_x) {
        put_be32(d + 12, rs); put_be32(d + 16, 1000);
        put_be32(d + 20, rs); put_be32(d + 24, 1000);
    }
    memset(&c, 0, sizeof(c));
    c.cdb[0] = 0xB6; c.cdb[8] = 0; c.cdb[9] = 28;
    c.cdb_len = 12; c.dir = ADSC_XFER_OUT;
    c.buf = d; c.buf_len = sizeof(d); c.timeout_ms = 10000;
    return adsc_dev_exec(dev, &c);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/sr0";
    int err = 0;
    accudisc_device *dev = accudisc_open(path, ACCUDISC_OPEN_RDWR, &err);
    if (!dev) { fprintf(stderr, "open: %s\n", accudisc_strerror(err)); return 1; }

    accudisc_toc toc;
    if (accudisc_read_toc(dev, &toc) == ACCUDISC_OK)
        printf("disc: %u tracks, leadout lba %u\n", toc.track_count,
               toc.leadout_lba);
    else
        printf("disc: no TOC\n");

    uint8_t cfg[64];
    if (adsc_mmc_get_configuration(dev, 0x0107, cfg, sizeof(cfg)) == ACCUDISC_OK)
        printf("profile 0x%04x\n", (uint16_t)(cfg[6] << 8 | cfg[7]));

    printf("== baseline ==\n");
    get_perf(dev, "nominal (untouched)");

    printf("== SET STREAMING flag-bit tests (no SET CD SPEED first) ==\n");
    int rc;
    rc = set_streaming(dev, 0x00, 8, 0, 0xFFFFFFFFu);
    sense_of(dev, rc, "flags=0x00 all-clear, 8x ceiling");
    get_perf(dev, "  -> perf after 8x ceiling");

    rc = set_streaming(dev, 0x40, 8, 0, 0xFFFFFFFFu);
    sense_of(dev, rc, "flags=0x40 (our current code)");

    rc = set_streaming(dev, 0x02, 8, 0, 0xFFFFFFFFu);
    sense_of(dev, rc, "flags=0x02 Exact -> CLV?");
    get_perf(dev, "  -> perf after Exact 8x");

    rc = set_streaming(dev, 0x02, 30, 0, 0xFFFFFFFFu);
    sense_of(dev, rc, "flags=0x02 Exact 30x (above inner)");

    rc = set_streaming(dev, 0x04, 0, 0, 0xFFFFFFFFu);
    sense_of(dev, rc, "flags=0x04 RDD (real restore)");
    get_perf(dev, "  -> perf after RDD");

    rc = set_streaming(dev, 0x20, 0, 0, 0xFFFFFFFFu);
    sense_of(dev, rc, "flags=0x20 (our current 'RDD')");

    printf("== ranged contract (Phase 3 preview) ==\n");
    rc = set_streaming(dev, 0x00, 4, 100000, 120000);
    sense_of(dev, rc, "4x over lba 100000..120000 only");
    get_perf(dev, "  -> perf after ranged 4x");

    rc = set_streaming(dev, 0x04, 0, 0, 0xFFFFFFFFu);
    sense_of(dev, rc, "RDD restore (cleanup)");

    accudisc_close(dev);
    return 0;
}
