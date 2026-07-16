/* Read-only medium probe: profile, RT-streaming bits, nominal performance
 * curve, disc info, TOC. Changes no drive state — safe on any disc (CD/DVD/BD).
 * Purpose: is the GET PERFORMANCE nominal curve medium-class-specific? */

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

static const char *profile_name(uint16_t p)
{
    switch (p) {
    case 0x0000: return "no current profile (no disc / unrecognised)";
    case 0x0008: return "CD-ROM";
    case 0x0009: return "CD-R";
    case 0x000A: return "CD-RW";
    case 0x0010: return "DVD-ROM";
    case 0x0011: return "DVD-R sequential";
    case 0x0012: return "DVD-RAM";
    case 0x0013: return "DVD-RW restricted overwrite";
    case 0x0014: return "DVD-RW sequential";
    case 0x0015: return "DVD-R DL sequential";
    case 0x0016: return "DVD-R DL layer jump";
    case 0x001A: return "DVD+RW";
    case 0x001B: return "DVD+R";
    case 0x002A: return "DVD+RW DL";
    case 0x002B: return "DVD+R DL";
    case 0x0040: return "BD-ROM";
    case 0x0041: return "BD-R SRM";
    case 0x0042: return "BD-R RRM";
    case 0x0043: return "BD-RE";
    case 0xFFFF: return "non-conforming disc";
    default:     return "unknown";
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/sr0";
    int err = 0;
    /* read-only: this probe changes nothing */
    accudisc_device *dev = accudisc_open(path, 0, &err);
    if (!dev) { fprintf(stderr, "open: %s\n", accudisc_strerror(err)); return 1; }

    uint8_t cfg[64];
    if (adsc_mmc_get_configuration(dev, 0x0107, cfg, sizeof(cfg)) == ACCUDISC_OK) {
        uint16_t p = (uint16_t)(cfg[6] << 8 | cfg[7]);
        printf("profile        0x%04x  %s\n", p, profile_name(p));
        uint16_t code = (uint16_t)(cfg[8] << 8 | cfg[9]);
        if (code == 0x0107 && cfg[11] >= 1) {
            uint8_t f = cfg[12];
            printf("rt_streaming   current=%u  SW=%u WSPD=%u MP2A=%u SCS=%u "
                   "RBCB=%u\n", cfg[10] & 1, (f >> 4) & 1, (f >> 3) & 1,
                   (f >> 2) & 1, (f >> 1) & 1, f & 1);
        }
    }

    unsigned maxk = 0, curk = 0;
    if (accudisc_get_speed(dev, &maxk, &curk) == ACCUDISC_OK)
        printf("page 2A        max %u kB/s (%.2fx)  current %u kB/s (%.2fx)\n",
               maxk, maxk / 176.4, curk, curk / 176.4);

    uint8_t di[34]; uint32_t dilen = 0;
    if (adsc_mmc_read_disc_info(dev, di, sizeof(di), &dilen) == ACCUDISC_OK
        && dilen >= 9)
        printf("disc info      status=%u erasable=%u sessions=%u last_trk=%u "
               "toc_type=0x%02x\n", di[2] & 0x03, (di[2] >> 4) & 1, di[4],
               di[6], di[8]);
    else
        printf("disc info      (unavailable)\n");

    accudisc_toc toc;
    if (accudisc_read_toc(dev, &toc) == ACCUDISC_OK) {
        int audio = 0, data = 0;
        for (uint8_t i = 0; i < toc.track_count; i++)
            ACCUDISC_TRACK_IS_AUDIO(&toc.tracks[i]) ? audio++ : data++;
        printf("toc            %u tracks (%d audio, %d data), leadout %u\n",
               toc.track_count, audio, data, toc.leadout_lba);
        printf("logical type   %s\n",
               data == 0 ? "CD-DA" : audio == 0 ? "CD-ROM (data)"
                                                : "Mixed Mode");
    } else {
        printf("toc            (none / not a CD)\n");
    }

    uint8_t pbuf[8 + 16 * 16];
    adsc_cmd c;
    memset(&c, 0, sizeof(c));
    memset(pbuf, 0, sizeof(pbuf));
    c.cdb[0] = 0xAC; c.cdb[9] = 0x10; c.cdb[10] = 0x00;
    c.cdb_len = 12; c.dir = ADSC_XFER_IN;
    c.buf = pbuf; c.buf_len = sizeof(pbuf); c.timeout_ms = 10000;
    int rc = adsc_dev_exec(dev, &c);
    if (rc != ACCUDISC_OK) {
        accudisc_sense s;
        accudisc_last_sense(dev, &s);
        printf("get perf       FAILED rc=%d key=0x%x asc=0x%02x\n", rc,
               s.key, s.asc);
    } else {
        uint32_t dlen = be32(pbuf);
        uint32_t n = dlen >= 4 ? (dlen - 4) / 16 : 0;
        printf("get perf       %u descriptor(s)\n", n);
        if (n > 16) n = 16;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *d = pbuf + 8 + i * 16;
            uint32_t sp = be32(d + 4), ep = be32(d + 12);
            printf("  desc[%u] lba %u..%u  %u..%u kB/s  CD:%.2fx..%.2fx  "
                   "DVD:%.2fx..%.2fx  %s\n", i, be32(d), be32(d + 8), sp, ep,
                   sp / 176.4, ep / 176.4, sp / 1385.0, ep / 1385.0,
                   sp == ep ? "FLAT (CLV)" : "RISING (CAV)");
        }
    }

    accudisc_close(dev);
    return 0;
}
