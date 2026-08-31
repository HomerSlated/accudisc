/* mmcspeed — measure DELIVERED CD-DA read throughput vs requested speed.
 * (vendor-driver zone; not built or shipped)
 *
 * The point of this tool is that mode page 2A reports the REQUEST, not what the
 * drive delivers, so the only honest measurement is a timed read.  It also
 * tests whether the delivered rate quantises onto the 12-step ladder found in
 * the firmware at 0xF65B83 (1 2 4 8 10 12 16 20 24 32 40 48x) by deliberately
 * requesting off-ladder rates.
 *
 * Method, and every part of it matters:
 *   - all trials read the SAME LBA range, so CAV radius is constant
 *   - that range is near the OUTER edge, where the drive can reach ~40x, so a
 *     speed cap is the binding constraint rather than the CAV curve
 *   - the 8 MiB buffer is evicted (distant read at max speed) before each trial
 *   - each trial reads > 8 MiB so the buffer cannot serve it
 *   - page 2A "current read speed" is read back only to CONTRAST it with the
 *     measured rate, never as the measurement
 *
 * Read-only: READ CD (0xBE) plus SET CD SPEED (0xBB) / SET STREAMING (0xB6).
 *   build: gcc -O2 -Wall -o mmcspeed mmcspeed.c
 *   run:   flock /var/tmp/sr0.lock ./mmcspeed /dev/sg3
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>

#define SECT   2352
#define PERCMD 26                  /* 26*2352 = 61152 < 64 KiB */
#define TRIAL_LBA   150000         /* set from the loaded disc's lead-out */
#define TRIAL_SECT  5000           /* 11.8 MB > 8 MiB buffer */
#define EVICT_LBA   20000
#define EVICT_SECT  4000

static int fd, scsi_status;
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
    io.timeout = 40000;
    if (ioctl(fd, SG_IO, &io) < 0) return -1;
    scsi_status = io.status;
    return blen - io.resid;
}

static int read_cdda(unsigned lba, unsigned nsect, unsigned char *buf)
{
    unsigned done = 0;
    while (done < nsect) {
        unsigned k = nsect - done; if (k > PERCMD) k = PERCMD;
        unsigned l = lba + done;
        unsigned char cdb[12] = { 0xbe, 0x04,
            (l>>24)&0xff, (l>>16)&0xff, (l>>8)&0xff, l&0xff,
            (k>>16)&0xff, (k>>8)&0xff, k&0xff, 0x10, 0, 0 };
        int n = cmd(cdb, 12, buf, k * SECT);
        if (n < 0 || scsi_status) return -1;
        done += k;
    }
    return 0;
}

static int set_cd_speed(unsigned rd_kbs)
{
    unsigned char cdb[12] = { 0xbb, 0,
        (rd_kbs>>8)&0xff, rd_kbs&0xff, 0xff, 0xff, 0,0,0,0,0,0 };
    cmd(cdb, 12, NULL, 0);
    return scsi_status;
}

/* SET STREAMING with a read performance descriptor demanding rd_kbs */
static int set_streaming(unsigned rd_kbs)
{
    unsigned char pd[28];
    memset(pd, 0, sizeof pd);
    pd[0] = 0x00;                      /* RDD=0, exact=0, RA=0 */
    /* start LBA 0 .. end LBA large */
    pd[4]=0; pd[5]=0; pd[6]=0; pd[7]=0;
    pd[8]=0x00; pd[9]=0x05; pd[10]=0x7e; pd[11]=0x3f;      /* end LBA */
    pd[12]=(rd_kbs>>24)&0xff; pd[13]=(rd_kbs>>16)&0xff;
    pd[14]=(rd_kbs>>8)&0xff;  pd[15]=rd_kbs&0xff;          /* read size */
    pd[16]=0; pd[17]=0; pd[18]=0x03; pd[19]=0xe8;          /* read time 1000ms */
    pd[20]=(rd_kbs>>24)&0xff; pd[21]=(rd_kbs>>16)&0xff;
    pd[22]=(rd_kbs>>8)&0xff;  pd[23]=rd_kbs&0xff;          /* write size */
    pd[24]=0; pd[25]=0; pd[26]=0x03; pd[27]=0xe8;
    /* parameter-list length is at CDB bytes 9-10, NOT 8-9. Getting this
     * wrong yields 5/24/00 and looks like "drive lacks SET STREAMING". */
    unsigned char cdb[12] = { 0xb6, 0, 0,0,0,0,0,0, 0, 0, 28, 0 };
    sg_io_hdr_t io;
    memset(&io, 0, sizeof io); memset(sense, 0, sizeof sense);
    io.interface_id='S'; io.cmd_len=12; io.cmdp=cdb;
    io.sbp=sense; io.mx_sb_len=sizeof sense;
    io.dxfer_direction=SG_DXFER_TO_DEV; io.dxferp=pd; io.dxfer_len=28;
    io.timeout=20000;
    if (ioctl(fd, SG_IO, &io) < 0) return -1;
    scsi_status = io.status;
    return scsi_status;
}

static int page2a_current(void)
{
    unsigned char b[256];
    unsigned char cdb[10] = { 0x5a, 0, 0x2a, 0, 0, 0, 0, 0, 254, 0 };
    int n = cmd(cdb, 10, b, 254);
    if (n < 24 || scsi_status) return -1;
    return (b[8 + 14] << 8) | b[8 + 15];      /* current read speed field */
}

static double now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/sg3";
    fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror("open"); return 2; }
    unsigned char *buf = malloc(PERCMD * SECT);
    unsigned char *ev  = malloc(PERCMD * SECT);
    if (!buf || !ev) return 2;

    struct { unsigned kbs; const char *note; int streaming; } cfg[] = {
        { 0xFFFF, "max (no cap)",           0 },
        { 5000,   "28.3x OFF ladder",       0 },
        { 4234,   "24x   ON ladder",        0 },
        { 3800,   "21.5x OFF -> pred 20x",  0 },
        { 3528,   "20x   ON ladder",        0 },
        { 3000,   "17.0x OFF -> pred 16x",  0 },
        { 2822,   "16x   ON ladder",        0 },
        { 2000,   "11.3x OFF -> pred 10x",  0 },
        { 1411,   "8x    ON ladder",        0 },
        { 1000,   "5.7x  OFF -> pred 4x",   0 },
        { 706,    "4x    ON ladder",        0 },
        { 0xFFFF, "max (repeat: drift)",    0 },
        { 2822,   "16x via SET STREAMING",  1 },
    };
    int nc = sizeof cfg / sizeof cfg[0];

    printf("LBA %u..%u  (%u sectors, %.1f MB per trial); same range every trial\n",
           TRIAL_LBA, TRIAL_LBA + TRIAL_SECT, TRIAL_SECT, TRIAL_SECT * (double)SECT / 1e6);
    printf("%-24s %8s %10s %8s %10s\n",
           "requested", "req x", "DELIVERED", "deliv x", "page2A x");
    for (int i = 0; i < nc; i++) {
        set_cd_speed(0xFFFF);
        if (read_cdda(EVICT_LBA, EVICT_SECT, ev) < 0) { printf("  evict failed\n"); continue; }
        int st = cfg[i].streaming ? set_streaming(cfg[i].kbs) : set_cd_speed(cfg[i].kbs);
        double t0 = now();
        int rc = read_cdda(TRIAL_LBA, TRIAL_SECT, buf);
        double el = now() - t0;
        if (rc < 0) { printf("  %-22s READ FAILED (status=%02x)\n", cfg[i].note, scsi_status); continue; }
        double kbs = TRIAL_SECT * (double)SECT / el / 1000.0;
        int p2a = page2a_current();
        printf("%-24s %8.1f %8.0f kB/s %7.1fx %9s%s\n",
               cfg[i].note,
               cfg[i].kbs == 0xFFFF ? 0.0 : cfg[i].kbs / 176.4,
               kbs, kbs / 176.4,
               p2a < 0 ? "n/a" : "", p2a < 0 ? "" : "");
        if (p2a >= 0) printf("%-24s %8s %8s      %7s %8.1fx  (page 2A reports the REQUEST)\n",
                             "", "", "", "", p2a / 176.4);
        if (st) printf("      (note: speed command returned status %02x)\n", st);
    }
    set_cd_speed(0xFFFF);
    close(fd);
    return 0;
}
