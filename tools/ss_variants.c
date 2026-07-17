/* SET STREAMING CDB parameter-list-length placement probe.
 *
 * The PX-716A rejects our SET STREAMING with 4/1b (SYNCHRONOUS DATA TRANSFER
 * ERROR). We write the 28-byte Parameter List Length at CDB bytes 8-9 (the
 * MMC-5 spec position). schily cdrecord writes it at bytes 9-10 with the comment
 * "Sz not G5 alike" (scsi_mmc.c:991). If the drive reads the length from 9-10,
 * our byte9=0x1C looks like a 0x1C00=7168-byte promise vs 28 sent -> underrun
 * -> 4/1b. This probe tries every plausible placement to find the one the drive
 * accepts. Read-only except the SET STREAMING data-OUT; needs CAP_SYS_RAWIO.
 * Restores drive defaults at the end. */

#include <stdio.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "internal.h"
#include "mmc/mmc.h"
#include "transport/transport.h"

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Build the 28-byte read-ceiling descriptor (flags=0x00, whole disc, speed_x).
 * write=0 leaves the Write Size/Time fields zero. */
static void build_desc(uint8_t d[28], unsigned speed_x, int write)
{
    uint32_t rs = speed_x * 1764u / 10u;
    memset(d, 0, 28);
    put_be32(d + 8, 0xFFFFFFFFu);           /* end LBA = whole disc */
    if (speed_x) {
        put_be32(d + 12, rs); put_be32(d + 16, 1000);
        if (write) { put_be32(d + 20, rs); put_be32(d + 24, 1000); }
    }
}

/* len_off: CDB byte index where the 2-byte (big-endian) param list length is
 * written. 8 = spec, 9 = schily. */
static int set_streaming_at(accudisc_device *dev, const uint8_t desc[28],
                            int len_off)
{
    adsc_cmd c;
    memset(&c, 0, sizeof(c));
    c.cdb[0] = 0xB6;
    c.cdb[len_off]     = (uint8_t)(28 >> 8);
    c.cdb[len_off + 1] = (uint8_t)(28 & 0xff);
    c.cdb_len = 12; c.dir = ADSC_XFER_OUT;
    c.buf = (void *)desc; c.buf_len = 28; c.timeout_ms = 10000;
    return adsc_dev_exec(dev, &c);
}

static void report(accudisc_device *dev, int rc, const char *what)
{
    accudisc_sense s;
    accudisc_last_sense(dev, &s);
    printf("  %-44s rc=%d", what, rc);
    if (rc == ACCUDISC_OK) printf("  OK");
    else if (s.valid) printf("  key=0x%x asc=0x%02x ascq=0x%02x", s.key, s.asc, s.ascq);
    putchar('\n');
}

static void show_speed(accudisc_device *dev, const char *tag)
{
    unsigned maxk = 0, curk = 0;
    if (accudisc_get_speed(dev, &maxk, &curk) == ACCUDISC_OK)
        printf("  %-44s page2A cur %.2fx  [max %.2fx]\n", tag,
               curk / 176.4, maxk / 176.4);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/sr0";
    int err = 0;
    accudisc_device *dev = accudisc_open(path, ACCUDISC_OPEN_RDWR, &err);
    if (!dev) { fprintf(stderr, "open: %s\n", accudisc_strerror(err)); return 1; }

    accudisc_toc toc;
    if (accudisc_read_toc(dev, &toc) == ACCUDISC_OK)
        printf("disc: %u tracks, leadout %u\n", toc.track_count, toc.leadout_lba);

    /* Force a known non-default speed first (via 0xBB) so a successful 0xB6
     * ceiling is visible as a change in page 2A. */
    accudisc_set_speed(dev, 40);
    show_speed(dev, "baseline (0xBB set 40x)");

    uint8_t desc[28];

    printf("== param-list-length placement, read+write desc, 8x ==\n");
    build_desc(desc, 8, 1);
    report(dev, set_streaming_at(dev, desc, 8), "len@8-9 (spec / our code)");
    report(dev, set_streaming_at(dev, desc, 9), "len@9-10 (schily 'not G5 alike')");
    show_speed(dev, "  -> page 2A after len@9-10");

    printf("== same, read-ONLY desc, 8x ==\n");
    build_desc(desc, 8, 0);
    report(dev, set_streaming_at(dev, desc, 8), "len@8-9 read-only");
    report(dev, set_streaming_at(dev, desc, 9), "len@9-10 read-only");
    show_speed(dev, "  -> page 2A after len@9-10 read-only");

    printf("== restore (RDD 0x04, len@9-10) ==\n");
    memset(desc, 0, 28);
    desc[0] = 0x04;
    report(dev, set_streaming_at(dev, desc, 9), "RDD restore");
    accudisc_set_speed(dev, 40);

    accudisc_close(dev);
    return 0;
}
