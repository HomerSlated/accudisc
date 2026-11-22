/* Does the PX-716A's USB bridge report a true READ CD residual? (cdda2img
 * correspondence 257, 2026-11-22.) 0.51.1 made READ CD's short-transfer guard
 * work, and C2+subchannel captures through the bridge then crawled: every
 * chunk ERR_SHORT, per-sector fallback, retries. Either the bridge reports a
 * residual for data that DID arrive (a known usb-storage failure, which is why
 * the kernel has the IGNORE_RESIDUE quirk), or the drive really sends less.
 *
 * Method: raw READ CD (adsc_dev_exec, no library post-processing) into a
 * buffer pre-filled with a sentinel byte, for each record format and a few
 * transfer sizes, exact length and rounded up to 16 as the library sends it.
 * For each: SCSI status, the residual reported, and the bytes that ARRIVED,
 * measured as the offset after the last non-sentinel byte (audio of a real
 * disc is never a long run of one byte value). Then the content check: the
 * audio fields of a C2+sub chunk against a plain 2352-byte read of the same
 * sectors. Read-only: READ CD only, no speed, no mode pages.
 *
 *   gcc -O2 -Wall -Wextra -o build/residprobe tools/residprobe.c \
 *       -I include -I src build/src/libaccudisc.a -ldl
 *   flock /var/tmp/sr0.lock timeout 120 ./build/residprobe /dev/sr0 LBA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "internal.h"

#define SENT 0xA5
#define MAXB (24 * 2742 + 16)

static uint8_t buf[MAXB], plain[24 * 2352];

static int rd(accudisc_device *dev, uint32_t lba, uint32_t n, uint8_t b9,
              uint8_t b10, uint8_t *dst, uint32_t len, uint32_t *resid,
              uint8_t *st, char *sense)
{
    adsc_cmd c;
    memset(&c, 0, sizeof c);
    c.cdb[0] = 0xBE;
    c.cdb[1] = 1u << 2; /* CD-DA */
    c.cdb[2] = (uint8_t)(lba >> 24); c.cdb[3] = (uint8_t)(lba >> 16);
    c.cdb[4] = (uint8_t)(lba >> 8);  c.cdb[5] = (uint8_t)lba;
    c.cdb[6] = (uint8_t)(n >> 16); c.cdb[7] = (uint8_t)(n >> 8);
    c.cdb[8] = (uint8_t)n;
    c.cdb[9] = b9;
    c.cdb[10] = b10;
    c.cdb_len = 12;
    c.dir = ADSC_XFER_IN;
    c.buf = dst;
    c.buf_len = len;
    c.timeout_ms = 30000;
    int rc = adsc_dev_exec(dev, &c);
    *resid = c.resid;
    *st = c.scsi_status;
    snprintf(sense, 16, "%x/%02x/%02x", c.sense_len > 2 ? c.sense[2] & 15 : 0,
             c.sense_len > 13 ? c.sense[12] : 0, c.sense_len > 13 ? c.sense[13] : 0);
    return rc;
}

static uint32_t arrived(const uint8_t *b, uint32_t len)
{
    uint32_t i = len;
    while (i > 0 && b[i - 1] == SENT)
        i--;
    return i;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s DEV LBA\n", argv[0]); return 2; }
    int err = 0;
    accudisc_device *dev = accudisc_open(argv[1], 0, &err);
    if (!dev) { fprintf(stderr, "open: %s\n", accudisc_strerror(err)); return 2; }
    uint32_t base = (uint32_t)strtoul(argv[2], NULL, 0);

    static const struct { const char *name; uint32_t rec; uint8_t b9, b10; } fmt[] = {
        {"audio      2352", 2352, 0x10, 0},
        {"audio+c2   2646", 2646, 0x12, 0},
        {"audio+sub  2448", 2448, 0x10, 1},
        {"audio+c2+sub 2742", 2742, 0x12, 1},
    };
    static const uint32_t ns[] = {1, 8, 23};

    printf("%-18s %4s %6s %6s | %-3s %6s %8s %8s\n", "format", "nsec", "exact",
           "sent", "st", "resid", "arrived", "missing");
    uint32_t lba = base;
    for (unsigned f = 0; f < 4; f++)
        for (unsigned k = 0; k < 3; k++)
            for (int round = 0; round < 2; round++) {
                uint32_t exact = ns[k] * fmt[f].rec;
                uint32_t len = round ? (exact + 15u) & ~15u : exact;
                if (round && len == exact)
                    continue;
                uint32_t resid; uint8_t st; char sense[16];
                memset(buf, SENT, sizeof buf);
                int rc = rd(dev, lba, ns[k], fmt[f].b9, fmt[f].b10, buf, len,
                            &resid, &st, sense);
                uint32_t got = arrived(buf, len);
                printf("%-18s %4u %6u %6u | %02x %6u %8u %8d%s%s\n", fmt[f].name,
                       ns[k], exact, len, st, resid, got, (int)exact - (int)got,
                       rc ? "  rc=" : "", rc ? sense : "");
                lba += 200; /* fresh sectors each time: no cache */
            }

    /* Content: a 23-sector C2+sub chunk (rounded, as the library sends it)
     * against a plain audio read of the same sectors. */
    uint32_t L = lba + 1000, resid; uint8_t st; char sense[16];
    memset(buf, SENT, sizeof buf);
    int rc1 = rd(dev, L, 23, 0x12, 1, buf, (23 * 2742 + 15u) & ~15u, &resid, &st, sense);
    uint32_t r1 = resid;
    rd(dev, L + 5000, 1, 0x10, 0, plain, 2352, &resid, &st, sense); /* move away */
    int rc2 = rd(dev, L, 23, 0x10, 0, plain, 23 * 2352, &resid, &st, sense);
    unsigned same = 0;
    for (unsigned s = 0; s < 23; s++)
        same += memcmp(buf + s * 2742, plain + s * 2352, 2352) == 0;
    printf("\ncontent @%u: c2+sub chunk rc=%d resid=%u arrived=%u of %u; plain rc=%d resid=%u;"
           " audio sectors identical: %u/23\n", L, rc1, r1,
           arrived(buf, (23 * 2742 + 15u) & ~15u), 23 * 2742, rc2, resid, same);
    accudisc_close(dev);
    return 0;
}
