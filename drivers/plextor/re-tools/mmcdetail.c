/* mmcdetail — read-only deep dump of the pages/features mmcsweep flags as
 * interesting (vendor-driver zone; not built or shipped).
 *
 * Follows mmcsweep by dumping payloads rather than presence:
 *   - full MODE SENSE(10) current + changeable for a named page list
 *   - GET CONFIGURATION feature descriptors WITH their payload bytes
 *   - READ BUFFER (0x3C) descriptors across buffer ids 0..15
 *   - GET PERFORMANCE (0xAC) type 3 (write speed descriptor list)
 *
 * All read-only. Never MODE SELECT, never WRITE BUFFER, no vendor opcodes.
 *   build: gcc -O2 -Wall -o mmcdetail mmcdetail.c
 *   run:   flock /var/tmp/sr0.lock ./mmcdetail /dev/sg3
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>

static int fd, scsi_status, sense_len;
static unsigned char sense[64];

static int cmd(const unsigned char *cdb, int len, unsigned char *buf, int blen)
{
    sg_io_hdr_t io;
    memset(&io, 0, sizeof io);
    memset(sense, 0, sizeof sense);
    io.interface_id = 'S'; io.cmd_len = len; io.cmdp = (unsigned char *)cdb;
    io.sbp = sense; io.mx_sb_len = sizeof sense;
    if (blen > 0) { io.dxfer_direction = SG_DXFER_FROM_DEV; io.dxferp = buf; io.dxfer_len = blen; }
    else io.dxfer_direction = SG_DXFER_NONE;
    io.timeout = 5000;
    if (ioctl(fd, SG_IO, &io) < 0) return -1;
    scsi_status = io.status; sense_len = io.sb_len_wr;
    return blen - io.resid;
}

static void hex(const unsigned char *p, int n, const char *ind)
{
    for (int i = 0; i < n; i++) {
        if (i % 16 == 0) printf("\n%s%04x:", ind, i);
        printf(" %02x", p[i]);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/sg3";
    unsigned char cur[256], chg[256], buf[8192];
    fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror("open"); return 2; }

    int pages[] = { 0x01, 0x08, 0x0d, 0x0e, 0x1a, 0x1d, 0x2a, -1 };
    for (int i = 0; pages[i] >= 0; i++) {
        int pg = pages[i];
        unsigned char cdb[10] = { 0x5a, 0, (unsigned char)pg, 0, 0, 0, 0, 0, 254, 0 };
        int nc = cmd(cdb, 10, cur, 254); int sc = scsi_status;
        cdb[2] = (unsigned char)(0x40 | pg);
        int ng = cmd(cdb, 10, chg, 254); int sg_ = scsi_status;
        if (nc < 10 || sc) { printf("\n== page 0x%02x: not supported ==\n", pg); continue; }
        int plen = cur[9] + 2;
        if (plen > nc - 8) plen = nc - 8;
        printf("\n== MODE PAGE 0x%02x (%d bytes) ==", pg, plen);
        printf("\n  current:  "); hex(cur + 8, plen, "    ");
        if (ng >= 10 && !sg_) { printf("  changeable:"); hex(chg + 8, plen, "    "); }
        else printf("  changeable: (PC=1 unsupported)\n");
    }

    printf("\n== GET CONFIGURATION feature payloads ==\n");
    unsigned char gc[10] = { 0x46, 0, 0, 0, 0, 0, 0, 0x10, 0x00, 0 };
    int n = cmd(gc, 10, buf, 4096);
    if (n >= 8 && !scsi_status) {
        int off = 8;
        while (off + 4 <= n) {
            int fc = (buf[off] << 8) | buf[off + 1];
            int ad = buf[off + 3], cur_f = buf[off + 2] & 1;
            int interesting = (fc == 0x0107 || fc == 0x0103 || fc == 0x010a ||
                               fc == 0x0000 || (fc & 0xff00) == 0xff00);
            if (interesting) {
                printf("  feature 0x%04x %s addl=%d", fc, cur_f ? "CURRENT" : "supported", ad);
                if (ad > 0 && off + 4 + ad <= n) hex(buf + off + 4, ad, "      ");
                else printf("\n");
            }
            off += 4 + ad;
            if (ad < 0) break;
        }
    } else printf("  FAILED status=%02x\n", scsi_status);

    printf("\n== READ BUFFER descriptors, ids 0..15 ==\n");
    for (int id = 0; id < 16; id++) {
        unsigned char rb[10] = { 0x3c, 0x03, (unsigned char)id, 0, 0, 0, 0, 0, 4, 0 };
        int r = cmd(rb, 10, buf, 4);
        if (r == 4 && !scsi_status) {
            unsigned cap = (buf[1] << 16) | (buf[2] << 8) | buf[3];
            if (cap) printf("  id=%2d  boundary=%u  capacity=%u\n", id, buf[0], cap);
        }
    }

    printf("\n== GET PERFORMANCE (0xAC) type=3 write-speed descriptors ==\n");
    unsigned char gp[12] = { 0xac, 0x00, 0, 0, 0, 0, 0, 0, 0, 16, 0x03, 0 };
    n = cmd(gp, 12, buf, 8 + 16 * 16);
    if (n >= 8 && !scsi_status) {
        int cnt = ((buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]) / 16;
        printf("  descriptors=%d\n", cnt);
        for (int i = 0; i < cnt && 8 + i * 16 + 16 <= n; i++) {
            unsigned char *d = buf + 8 + i * 16;
            unsigned rd = (d[8] << 24) | (d[9] << 16) | (d[10] << 8) | d[11];
            unsigned wr = (d[12] << 24) | (d[13] << 16) | (d[14] << 8) | d[15];
            printf("    [%d] flags=%02x  read=%u kB/s (%.1fx)  write=%u kB/s (%.1fx)\n",
                   i, d[0], rd, rd / 176.4, wr, wr / 176.4);
        }
    } else printf("  status=%02x n=%d\n", scsi_status, n);

    close(fd);
    return 0;
}
