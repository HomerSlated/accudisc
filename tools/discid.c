/* Identify a disc the drive calls BLANK.
 *
 * "A disc that reads blank may be written" is already a standing rule in this
 * project (a burn is only verified after an eject and reload), and on
 * 2026-09-12 it stopped being an abstraction: several discs that had read as
 * blank for two days came back and mostly played. So `blank` is the drive's
 * opinion, and this tool exists to go looking for a second one.
 *
 * Four routes, cheapest first, and they fail independently — the point is that
 * a disc which defeats the first three can still be identified by the fourth:
 *
 *   1. READ DISC INFORMATION (0x51). Status, last-session state, and the
 *      lead-in/lead-out of the last session. Answers from the drive's model of
 *      the disc rather than from the lead-in itself.
 *   2. READ TOC/PMA/ATIP (0x43) in all five formats. Format 0 is the one that
 *      says "blank"; the interesting ones are 2 (the RAW lead-in, which can
 *      survive when the cooked TOC does not) and especially 3, the PMA — the
 *      Program Memory Area, where a recorder tracks what it has written BEFORE
 *      the lead-in is finalised. A disc whose lead-in never took can still
 *      have a PMA. Format 4 is ATIP, which works on blank media and names the
 *      dye manufacturer: the one discriminator that is independent of whether
 *      anything was recorded at all.
 *   3. READ TRACK INFORMATION (0x52), for track 1 and for the "invisible"
 *      track. Carries the track's start and its next writable address, i.e. a
 *      LENGTH, which is the single most identifying number available for a
 *      disc whose content we already know the size of.
 *   4. Raw READ CD (0xBE) at a spread of LBAs, in both CD-DA and "any" sector
 *      flavours. This is the decisive one. If sectors come back with content,
 *      the disc IS written whatever the TOC says, and the bytes themselves say
 *      what it is: silence, white noise, music, or a filesystem.
 *
 * Entirely read-only. Nothing here writes, erases, or changes drive state.
 *
 *   cmake --build build
 *   gcc -O2 -o build/discid tools/discid.c -I include -I src \
 *       build/src/libaccudisc.a -ldl
 *   doas /usr/bin/setcap cap_sys_rawio=ep build/discid
 *   flock /var/tmp/sr0.lock ./build/discid /dev/sr0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "internal.h"
#include "mmc/cdb.h"
#include "mmc/mmc.h"

static const char *rcstr(accudisc_device *dev, int rc)
{
    static char buf[64];
    accudisc_sense s;

    if (rc == ACCUDISC_OK)
        return "ok";
    accudisc_last_sense(dev, &s);
    if (s.valid)
        snprintf(buf, sizeof buf, "%u/%02X/%02X", s.key, s.asc, s.ascq);
    else
        snprintf(buf, sizeof buf, "rc=%d", rc);
    return buf;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8
           | p[3];
}

/* A 2 KiB sector's character, which is what actually identifies the content
 * when no TOC survives. Deliberately crude and deliberately three-way: the
 * question is only "silence, noise, or structure?", and anything finer would
 * be a claim this tool cannot support. */
static void describe(const uint8_t *b, uint32_t n)
{
    uint32_t zero = 0, hist[256] = {0}, distinct = 0;
    double mean = 0.0;

    for (uint32_t i = 0; i < n; i++) {
        if (!b[i])
            zero++;
        hist[b[i]]++;
    }
    for (unsigned i = 0; i < 256; i++)
        if (hist[i])
            distinct++;
    for (uint32_t i = 0; i < n; i++)
        mean += b[i];
    mean /= n;

    printf("      %u/%u zero, %u distinct byte values, mean %.1f  ", zero, n,
           distinct, mean);
    if (zero == n)
        printf("=> ALL ZERO (digital silence, or an unwritten sector read as "
               "zeros)\n");
    else if (zero > n * 9 / 10)
        printf("=> nearly all zero (silence with something in it)\n");
    else if (distinct > 240)
        printf("=> high-entropy (white noise, compressed data, or encrypted)\n");
    else
        printf("=> structured (text, a filesystem, or low-entropy audio)\n");

    printf("      first 32 bytes:");
    for (uint32_t i = 0; i < 32 && i < n; i++)
        printf("%s%02x", (i % 16) ? " " : "\n        ", b[i]);
    putchar('\n');
    /* ISO9660 and a few other signatures are worth naming outright: they turn
     * "structured" into an identification. */
    if (n >= 6 && !memcmp(b + 1, "CD001", 5))
        printf("      ** ISO9660 volume descriptor (type %u) **\n", b[0]);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/sr0";
    accudisc_device *dev;
    uint8_t *toc = NULL;
    uint32_t len = 0;
    int err = 0, rc;

    dev = accudisc_open(path, 0, &err);
    if (!dev) {
        fprintf(stderr, "discid: cannot open %s (err %d)\n", path, err);
        return 1;
    }

    /* ---- 1. READ DISC INFORMATION ---------------------------------------- */
    {
        uint8_t di[34];
        uint32_t dlen = 0;

        printf("== READ DISC INFORMATION (0x51)\n");
        rc = adsc_mmc_read_disc_info(dev, di, sizeof di, &dlen);
        printf("   %s", rcstr(dev, rc));
        if (rc == ACCUDISC_OK && dlen >= 22) {
            printf("  status=%u (0=blank 1=incomplete 2=complete)"
                   " last_session=%u erasable=%u\n",
                   di[2] & 0x03, (di[2] >> 2) & 0x03, (di[2] >> 4) & 1);
            printf("   first_track=%u  sessions=%u  last_session_first_track=%u"
                   "  last_session_last_track=%u\n", di[3], di[4], di[5],
                   di[6]);
            printf("   last_session_leadin=0x%08x  last_leadout=0x%08x\n",
                   be32(di + 16), be32(di + 20));
        } else {
            putchar('\n');
        }
    }

    /* ---- 2. every TOC/PMA/ATIP format ------------------------------------ */
    {
        static const struct { unsigned fmt; const char *name; } f[] = {
            { 0, "TOC (the format that says \"blank\")" },
            { 1, "session info" },
            { 2, "FULL TOC — the RAW lead-in" },
            { 3, "PMA — written BEFORE the lead-in is finalised" },
            { 4, "ATIP — works on blank media; names the dye" },
        };

        for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
            printf("\n== READ TOC/PMA/ATIP format %u — %s\n", f[i].fmt,
                   f[i].name);
            rc = adsc_mmc_read_toc_raw(dev, f[i].fmt, 0, 0, &toc, &len);
            printf("   %s", rcstr(dev, rc));
            if (rc == ACCUDISC_OK && len >= 4) {
                printf("  %u bytes\n  ", len);
                for (uint32_t b = 0; b < len && b < 128; b++)
                    printf("%s%02x", (b && b % 24 == 0) ? "\n  " : " ", toc[b]);
                if (len > 128)
                    printf(" ... (%u more)", len - 128);
                putchar('\n');
            } else {
                putchar('\n');
            }
            free(toc);
            toc = NULL;
        }
    }

    /* ---- 3. READ TRACK INFORMATION --------------------------------------- */
    {
        static const struct { uint8_t type; uint32_t num; const char *what; } t[] = {
            { 0x01, 1, "track 1" },
            { 0x01, 0xFF, "the invisible/incomplete track" },
        };

        for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
            adsc_cmd cmd;
            uint8_t ti[48] = {0};

            printf("\n== READ TRACK INFORMATION (0x52) — %s\n", t[i].what);
            memset(&cmd, 0, sizeof cmd);
            cmd.cdb[0] = 0x52;
            cmd.cdb[1] = t[i].type;
            cmd.cdb[2] = (uint8_t)(t[i].num >> 24);
            cmd.cdb[3] = (uint8_t)(t[i].num >> 16);
            cmd.cdb[4] = (uint8_t)(t[i].num >> 8);
            cmd.cdb[5] = (uint8_t)t[i].num;
            cmd.cdb[7] = 0;
            cmd.cdb[8] = sizeof ti;
            cmd.cdb_len = 10;
            cmd.dir = ADSC_XFER_IN;
            cmd.buf = ti;
            cmd.buf_len = sizeof ti;
            cmd.timeout_ms = 15000;
            rc = adsc_dev_exec(dev, &cmd);
            printf("   %s", rcstr(dev, rc));
            if (rc == ACCUDISC_OK) {
                /* SIGNED. MMC logical block addresses are signed 32-bit and
                 * the start of the programme area is LBA -150 (MSF 00:00:00),
                 * so an untouched disc legitimately reports next_writable as
                 * 0xFFFFFF6A. Read unsigned, that is 4294967146, and the first
                 * version of this tool duly announced "4294967146 sectors
                 * appear to be RECORDED (954437:08:46)" for a disc with
                 * nothing on it. Absurd enough to catch by eye here; the same
                 * mistake on a plausible-looking number would not have been. */
                int32_t start = (int32_t)be32(ti + 8),
                        nwa = (int32_t)be32(ti + 12),
                        size = (int32_t)be32(ti + 24);

                printf("\n   track_start=%d  next_writable=%d  track_size=%d\n",
                       start, nwa, size);
                printf("   damage=%u copy=%u blank=%u packet=%u nwa_valid=%u\n",
                       (ti[5] >> 5) & 1, (ti[5] >> 4) & 1, (ti[6] >> 6) & 1,
                       (ti[6] >> 5) & 1, ti[7] & 1);
                /* The length is the identification. A burn's extent is far more
                 * specific than anything else recoverable from a disc whose
                 * lead-in is gone. */
                if (nwa <= 0)
                    printf("   => next writable address is the START of the "
                           "programme area (LBA %d): the drive believes "
                           "NOTHING is recorded\n", nwa);
                else if (nwa > start)
                    printf("   => %d sectors appear to be RECORDED "
                           "(%d:%02d:%02d)\n", nwa - start,
                           (nwa - start) / 75 / 60, (nwa - start) / 75 % 60,
                           (nwa - start) % 75);
            } else {
                putchar('\n');
            }
        }
    }

    /* ---- 4. raw reads: the decisive test --------------------------------- */
    {
        static const uint32_t at[] = { 0, 100, 5000, 50000, 150000, 300000 };
        uint8_t *buf = malloc(ACCUDISC_BYTES_AUDIO);

        printf("\n== RAW READ CD (0xBE) — does anything come back at all?\n");
        printf("   A disc the TOC calls blank but that returns content here "
               "IS written.\n");
        if (!buf) {
            printf("   out of memory\n");
        } else {
            for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
                int got_any = 0;

                for (unsigned mode = 0; mode < 2; mode++) {
                    memset(buf, 0, ACCUDISC_BYTES_AUDIO);
                    rc = adsc_mmc_read_cd(dev, at[i], 1,
                                          mode ? ADSC_SECTOR_ANY
                                               : ADSC_SECTOR_CDDA,
                                          ACCUDISC_C2_NONE, ACCUDISC_SUB_NONE,
                                          buf, ACCUDISC_BYTES_AUDIO);
                    printf("   LBA %7u  %-4s  %s\n", at[i],
                           mode ? "any" : "cdda", rcstr(dev, rc));
                    if (rc == ACCUDISC_OK && !got_any) {
                        describe(buf, ACCUDISC_BYTES_AUDIO);
                        got_any = 1;
                    }
                }
            }
            free(buf);
        }
    }

    accudisc_close(dev);
    return 0;
}
