/* mmcsweep — read-only MMC capability sweep (vendor-driver zone; not built or shipped).
 *
 * Enumerates what a drive actually exposes to the host, using only read-only
 * commands, so it is safe to run against a loaded or empty drive:
 *
 *   INQUIRY (0x12)            drive identity
 *   TEST UNIT READY (0x00)    media / tray state, via sense
 *   MODE SENSE(10) (0x5A)     every page code 0x00-0x3F at PC=0..3.
 *                             PC=1 is the CHANGEABLE-VALUES MASK: a set bit is
 *                             the drive stating the host may alter that bit.
 *                             That mask is the exposed control surface, read
 *                             from the drive rather than inferred.
 *   GET CONFIGURATION (0x46)  full MMC feature list (RT=0)
 *   READ BUFFER (0x3C)        mode 3 (descriptor) then mode 2 (data)
 *
 * Never issues WRITE BUFFER (0x3B), MODE SELECT, or any vendor opcode.
 *
 *   build: gcc -O2 -Wall -o mmcsweep mmcsweep.c
 *   run:   flock /var/tmp/sr0.lock ./mmcsweep /dev/sg3
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>

static int fd;
static unsigned char sense[64];
static int sense_len, scsi_status;

/* returns bytes transferred, or -1 on ioctl failure */
static int cmd(const unsigned char *cdb, int cdblen, unsigned char *buf, int buflen)
{
    sg_io_hdr_t io;
    memset(&io, 0, sizeof io);
    memset(sense, 0, sizeof sense);
    io.interface_id = 'S';
    io.cmd_len = cdblen;
    io.cmdp = (unsigned char *)cdb;
    io.sbp = sense;
    io.mx_sb_len = sizeof sense;
    if (buflen > 0) { io.dxfer_direction = SG_DXFER_FROM_DEV; io.dxferp = buf; io.dxfer_len = buflen; }
    else            { io.dxfer_direction = SG_DXFER_NONE; }
    io.timeout = 5000;
    if (ioctl(fd, SG_IO, &io) < 0) return -1;
    scsi_status = io.status;
    sense_len = io.sb_len_wr;
    return buflen - io.resid;
}

static void sense_kcq(int *k, int *c, int *q)
{
    *k = *c = *q = -1;
    if (sense_len < 3) return;
    if ((sense[0] & 0x7e) == 0x72) { *k = sense[1] & 0xf; *c = sense[2]; *q = sense[3]; }
    else if (sense_len >= 14)      { *k = sense[2] & 0xf; *c = sense[12]; *q = sense[13]; }
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/sg3";
    unsigned char buf[8192];
    int n, k, c, q;

    fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror("open"); return 2; }

    /* ---- identity ---- */
    unsigned char inq[6] = { 0x12, 0, 0, 0, 96, 0 };
    n = cmd(inq, 6, buf, 96);
    printf("== INQUIRY ==\n");
    if (n >= 36) printf("  vendor='%.8s' product='%.16s' rev='%.4s'\n", buf + 8, buf + 16, buf + 32);
    else printf("  FAILED (n=%d status=%02x)\n", n, scsi_status);

    /* ---- media state ---- */
    unsigned char tur[6] = { 0, 0, 0, 0, 0, 0 };
    cmd(tur, 6, NULL, 0);
    sense_kcq(&k, &c, &q);
    printf("== TEST UNIT READY == status=%02x sense=%d/%02x/%02x %s\n", scsi_status, k, c, q,
           scsi_status == 0 ? "(media present & ready)" :
           (c == 0x3a ? "(NO MEDIUM - tray empty)" : "(not ready / other)"));

    /* ---- MODE SENSE(10) sweep ---- */
    printf("\n== MODE SENSE(10) page sweep ==\n");
    printf("   page  len  PC0(current)                PC1(changeable mask)\n");
    int npages = 0, nchange = 0;
    for (int pg = 0; pg <= 0x3f; pg++) {
        unsigned char cur[256], chg[256];
        unsigned char cdb[10] = { 0x5a, 0, (unsigned char)pg, 0, 0, 0, 0, 0, 254, 0 };
        int nc = cmd(cdb, 10, cur, 254);
        int st_cur = scsi_status;
        if (nc < 10 || st_cur != 0) continue;          /* page not supported */
        npages++;
        cdb[2] = (unsigned char)(0x40 | pg);           /* PC=1 changeable */
        int ng = cmd(cdb, 10, chg, 254);
        int plen = cur[9] < 250 ? cur[9] : 250;        /* page length byte */
        printf("   0x%02x  %3d  ", pg, plen);
        for (int i = 0; i < 8 && i < plen; i++) printf("%02x ", cur[10 + i]);
        printf("  |  ");
        int any = 0;
        if (ng >= 10 && scsi_status == 0) {
            for (int i = 0; i < 8 && i < plen; i++) { printf("%02x ", chg[10 + i]); if (chg[10 + i]) any = 1; }
            for (int i = 0; i < plen && i + 10 < ng; i++) if (chg[10 + i]) any = 1;
        } else printf("(PC=1 unsupported)");
        if (any) { printf(" <== HOST-CHANGEABLE"); nchange++; }
        printf("\n");
    }
    printf("   -> %d mode pages present, %d with a non-zero changeable mask\n", npages, nchange);

    /* ---- GET CONFIGURATION ---- */
    printf("\n== GET CONFIGURATION (RT=0, all features) ==\n");
    unsigned char gc[10] = { 0x46, 0, 0, 0, 0, 0, 0, 0x10, 0x00, 0 };
    n = cmd(gc, 10, buf, 4096);
    if (n < 8 || scsi_status) { printf("  FAILED status=%02x n=%d\n", scsi_status, n); }
    else {
        int off = 8, nf = 0;
        while (off + 4 <= n) {
            int fc = (buf[off] << 8) | buf[off + 1];
            int cur = buf[off + 2] & 1, ver = (buf[off + 2] >> 2) & 0xf;
            int ad = buf[off + 3];
            printf("  feature 0x%04x  ver=%d %s\n", fc, ver, cur ? "CURRENT" : "supported");
            off += 4 + ad; nf++;
            if (ad < 0 || nf > 200) break;
        }
        printf("   -> %d features reported\n", nf);
    }

    /* ---- READ BUFFER ---- */
    printf("\n== READ BUFFER (0x3C) ==\n");
    for (int id = 0; id < 4; id++) {
        unsigned char rb[10] = { 0x3c, 0x03, (unsigned char)id, 0, 0, 0, 0, 0, 4, 0 };
        n = cmd(rb, 10, buf, 4);
        sense_kcq(&k, &c, &q);
        if (n == 4 && scsi_status == 0) {
            unsigned cap = (buf[1] << 16) | (buf[2] << 8) | buf[3];
            printf("  descriptor id=%d: offset_boundary=%u capacity=%u bytes\n", id, buf[0], cap);
        } else {
            printf("  descriptor id=%d: status=%02x sense=%d/%02x/%02x\n", id, scsi_status, k, c, q);
        }
    }
    unsigned char rbd[10] = { 0x3c, 0x02, 0, 0, 0, 0, 0, 0, 64, 0 };
    n = cmd(rbd, 10, buf, 64);
    sense_kcq(&k, &c, &q);
    printf("  data mode(2) id=0 len=64: n=%d status=%02x sense=%d/%02x/%02x\n", n, scsi_status, k, c, q);
    if (n > 0 && scsi_status == 0) {
        printf("   ");
        for (int i = 0; i < n && i < 64; i++) { if (i && i % 16 == 0) printf("\n   "); printf("%02x ", buf[i]); }
        printf("\n   -> READ BUFFER data mode RETURNED DATA (possible live memory window)\n");
    }
    close(fd);
    return 0;
}
