/* cxdump — dump the RAW 26-byte Plextor 0xEA/0x16 CD counter readout, and test
 * whether the drive's own BLER field equals our E11+E21+E31 sum.
 * (vendor-driver zone; not built or shipped)
 *
 * WHY THIS EXISTS.  drivers/plextor/plextor.c decodes THREE values out of the
 * 26-byte block: C1 as the sum of [12..17], CU from [20..21], C2 from
 * [22..23].  QPxTool's qscan_cmd.cpp decodes EIGHT, and places them
 * differently -- bler at 10, e31/e21/e11 at 12/14/16, uncr at 18, then
 * e32/e22/e12 at 20/22/24.  The two disagree about where "uncorrectable"
 * lives, and QPxTool's own source is unsure: lines 273-274 carry the comments
 * "// check where drive returns E32" and "// and where is UNCR".  Neither side
 * is authoritative (docs/reference/OPCODES.md, the open-question note under
 * the 0xEA table).
 *
 * That ambiguity blocks any pass/fail verdict built on C2, because C2 == 0 is
 * exactly the criterion a good burn is judged by.  A verdict reading the wrong
 * offset is well-formed and wrong -- the project's named failure mode.
 *
 * WHAT IT SETTLES, AND WHAT IT DOES NOT.
 *   SETTLED, for free, on any readable disc:  every C1 block error carries 1,
 *   2 or 3 bad symbols, so if the drive's BLER field equals e11+e21+e31 the
 *   C1 frame is anchored and plextor.c's C1 is the quantity a BLER limit
 *   refers to.  A mismatch is equally informative: it would mean the fields
 *   are sampled independently and the sum is NOT that quantity.
 *
 *   NOT SETTLED here:  the C2/CU offsets.  On a clean disc bytes 18/20/22/24
 *   are all zero and EVERY candidate assignment "passes" -- the inputs cannot
 *   distinguish the hypotheses, so the test would be worth nothing.  Telling
 *   those apart needs a disc with real C2 activity, where the expected
 *   relationship e12 >= e22 >= e32 discriminates.  This tool prints all four
 *   so that disc, if one is ever scanned, answers it in one pass.
 *
 * THIS IS A FRAMING QUESTION, NOT A RECOVERY EXPERIMENT.  It identifies which
 * byte offset holds which counter in a fixed-length response.  It does not
 * measure, compare or select on recovery outcomes, and no conclusion here
 * depends on the run-to-run variability of a damaged read.
 *
 * SAFETY.  Issues only 0xEA sub-commands 0x15/0x16/0x17 (already shipped in
 * plextor.c, read-only) and 0xBE READ CD (read-only).  Nothing here writes.
 * It still gates on the MEDIUM and refuses a blank, per the standing rule --
 * on a blank there is nothing to read and the counters would be meaningless.
 *
 *   build: gcc -O2 -Wall -Wextra -o cxdump cxdump.c
 *   run:   flock /var/tmp/sr0.lock ./cxdump /dev/sr0 [start_lba] [intervals]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>

#define CX_LEN      26
#define CADENCE     75      /* one audio second; matches ACCUDISC_CENSUS_CADENCE */
#define RAW_SECTOR  2352

static int fd, st, slen;
static unsigned char sn[64];

/* Trace every CDB before issuing it, unbuffered -- the project rule.  These
 * opcodes are known-safe, but a durable trace costs nothing and a lost one
 * has cost this project a diagnosis before. */
static int cmd(const unsigned char *c, int l, unsigned char *b, int n, int ms)
{
    sg_io_hdr_t io;

    fprintf(stderr, "CDB:");
    for (int i = 0; i < l; i++) fprintf(stderr, " %02x", c[i]);
    fprintf(stderr, "\n"); fflush(stderr);

    memset(&io, 0, sizeof io); memset(sn, 0, sizeof sn);
    io.interface_id = 'S'; io.cmd_len = (unsigned char)l;
    io.cmdp = (unsigned char *)c;
    io.sbp = sn; io.mx_sb_len = sizeof sn;
    if (n > 0) {
        io.dxfer_direction = SG_DXFER_FROM_DEV; io.dxferp = b; io.dxfer_len = n;
    } else {
        io.dxfer_direction = SG_DXFER_NONE;
    }
    io.timeout = ms;
    if (ioctl(fd, SG_IO, &io) < 0) { perror("SG_IO"); return -1; }
    st = io.status; slen = io.sb_len_wr;
    if (st) {
        fprintf(stderr, "  status %02x sense", st);
        for (int i = 0; i < slen && i < 18; i++) fprintf(stderr, " %02x", sn[i]);
        fprintf(stderr, "\n");
        return -1;
    }
    return n - (int)io.resid;
}

static unsigned be16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

/* READ DISC INFORMATION -- the medium gate.  Byte 2 bits 1:0 are the disc
 * status; 0 == EMPTY (blank).  Refusing here is not about 0xEA being unsafe,
 * it is that a scan of a blank produces zeros that would look like a clean
 * disc to anyone reading the output later. */
static int disc_is_blank(void)
{
    unsigned char cdb[10] = {0x51, 0, 0, 0, 0, 0, 0, 0, 34, 0};
    unsigned char d[34] = {0};

    if (cmd(cdb, 10, d, sizeof d, 20000) < 0) return -1;
    return (d[2] & 0x03) == 0;
}

int main(int argc, char **argv)
{
    unsigned start = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 0;
    int want = argc > 3 ? atoi(argv[3]) : 10;
    unsigned char *buf = malloc((size_t)CADENCE * RAW_SECTOR);
    int agree = 0, differ = 0, allzero = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s /dev/srN [start_lba] [intervals]\n", argv[0]);
        return 2;
    }
    if (!buf) { perror("malloc"); return 1; }
    if ((fd = open(argv[1], O_RDONLY | O_NONBLOCK)) < 0) { perror(argv[1]); return 1; }

    switch (disc_is_blank()) {
    case 1:
        fprintf(stderr, "REFUSED: the loaded disc is BLANK. Load a recorded "
                        "disc -- a blank yields all-zero counters that would "
                        "read as a clean scan.\n");
        return 3;
    case -1:
        fprintf(stderr, "REFUSED: could not read disc information, so the "
                        "medium is unknown.\n");
        return 3;
    default:
        break;
    }

    /* 0xEA 0x15 -- arm CD error counters (CDB[3]=0x01 selects CD). */
    {
        unsigned char cdb[12] = {0xEA, 0x15, 0x00, 0x01, 0,0,0,0,0,0,0,0};
        if (cmd(cdb, 12, NULL, 0, 30000) < 0) {
            fprintf(stderr, "arm refused -- no counter support on this drive\n");
            return 1;
        }
    }

    printf("# lba  bler  e31  e21  e11 | b18  e32/b20  e22/b22  e12/b24 | "
           "sum(e11+e21+e31)  bler==sum\n");

    for (int i = 0; i < want; i++) {
        unsigned lba = start + (unsigned)i * CADENCE;
        unsigned char rd[12] = {0xBE, 0x00, 0,0,0,0, 0, 0, CADENCE, 0xF8, 0, 0};
        unsigned char d[CX_LEN] = {0};
        unsigned char cx[12] = {0xEA, 0x16, 0x01, 0,0,0,0,0,0,0, CX_LEN, 0};
        unsigned bler, e31, e21, e11, sum;

        rd[2] = (unsigned char)(lba >> 24); rd[3] = (unsigned char)(lba >> 16);
        rd[4] = (unsigned char)(lba >> 8);  rd[5] = (unsigned char)lba;
        /* A failed read is NOT fatal: the counters are the point, and an
         * unreadable span still has counters worth seeing.  It is reported. */
        if (cmd(rd, 12, buf, CADENCE * RAW_SECTOR, 60000) < 0)
            fprintf(stderr, "  (read failed at lba %u -- counters still sampled)\n", lba);

        if (cmd(cx, 12, d, CX_LEN, 30000) < 0) break;

        fprintf(stderr, "  raw:");
        for (int b = 0; b < CX_LEN; b++) fprintf(stderr, " %02x", d[b]);
        fprintf(stderr, "\n");

        bler = be16(d + 10); e31 = be16(d + 12);
        e21  = be16(d + 14); e11 = be16(d + 16);
        sum  = e31 + e21 + e11;

        printf("%7u %5u %4u %4u %4u | %3u %8u %8u %8u | %5u  %s\n",
               lba, bler, e31, e21, e11,
               be16(d + 18), be16(d + 20), be16(d + 22), be16(d + 24),
               sum, bler == sum ? "YES" : "no");

        if (bler == 0 && sum == 0) allzero++;
        else if (bler == sum)      agree++;
        else                       differ++;
    }

    { unsigned char cdb[12] = {0xEA, 0x17, 0,0,0,0,0,0,0,0,0,0};
      cmd(cdb, 12, NULL, 0, 30000); }

    printf("\n# intervals: %d agree, %d differ, %d both-zero (uninformative)\n",
           agree, differ, allzero);
    if (agree + differ == 0)
        printf("# VERDICT: NONE. Every interval was zero on both sides, which\n"
               "#   cannot distinguish the hypotheses. Scan a span with real\n"
               "#   C1 activity -- a clean disc proves nothing here.\n");
    else if (differ == 0)
        printf("# VERDICT: bler == e11+e21+e31 on all %d informative intervals.\n"
               "#   plextor.c's C1 is the drive's own BLER. Frame anchored.\n", agree);
    else
        printf("# VERDICT: MISMATCH on %d of %d informative intervals. The sum\n"
               "#   is NOT the drive's BLER -- do not treat plextor.c's c1 as a\n"
               "#   BLER figure until this is explained.\n", differ, agree + differ);
    return 0;
}
