/* mmcvendor — probe the undocumented Plextor opcodes 0xD9 / 0xF2 / 0xF4
 * against whatever medium is loaded, and map which CDB fields each validates.
 * (vendor-driver zone; not built or shipped)
 *
 * Every result we have on these opcodes so far was taken against ONE profile
 * (0x0008, CD-ROM).  Two of the three failed with sense codes that are media or
 * track-mode rejections rather than CDB errors, so the medium is a variable
 * that has to be swept, not held constant.
 *
 * SAFETY, and it is enforced here rather than left to the operator:
 *   NO opcode of unknown function is issued against a WRITABLE medium unless
 *   the operator passes --allow-write-probe to say the disc is expendable.
 *
 *   This gate originally covered only 0xF2, because 0xF2's sense code
 *   (2/30/05 CANNOT WRITE MEDIUM) proved it was write-side while 0xF4 merely
 *   rejected a CDB field and looked inert.  That reasoning was wrong: on a
 *   pressed CD 0xF4 returns 5/24/00, but on a CD-R it returns GOOD STATUS --
 *   i.e. it executes.  An opcode that looks harmless against read-only media
 *   tells you nothing about what it does against writable media, which is the
 *   whole reason the gate exists.  Gate on the MEDIUM, never on a per-opcode
 *   guess about which ones are dangerous.  (Learned 2026-08-31, on a disc that
 *   turned out to be a CD-R when a pressed CD-ROM was expected.)
 *
 *   build: gcc -O2 -Wall -o mmcvendor mmcvendor.c
 *   run:   flock /var/tmp/sr0.lock ./mmcvendor /dev/sg3 [--allow-write-probe]
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>

static int fd, st, slen;
static unsigned char sn[64];

static int cmd(const unsigned char *c, int l, unsigned char *b, int n)
{
    sg_io_hdr_t io;
    memset(&io, 0, sizeof io); memset(sn, 0, sizeof sn);
    io.interface_id = 'S'; io.cmd_len = l; io.cmdp = (unsigned char *)c;
    io.sbp = sn; io.mx_sb_len = sizeof sn;
    if (n > 0) { io.dxfer_direction = SG_DXFER_FROM_DEV; io.dxferp = b; io.dxfer_len = n; }
    else io.dxfer_direction = SG_DXFER_NONE;
    io.timeout = 20000;
    if (ioctl(fd, SG_IO, &io) < 0) return -1;
    st = io.status; slen = io.sb_len_wr;
    return n - io.resid;
}

static void kcq(int *k, int *a, int *q)
{
    *k = *a = *q = -1;
    if (slen >= 14) { *k = sn[2] & 0xf; *a = sn[12]; *q = sn[13]; }
}

static const char *ascq_name(int a, int q)
{
    if (a == 0x20) return "INVALID COMMAND OPERATION CODE";
    if (a == 0x24) return "INVALID FIELD IN CDB";
    if (a == 0x64) return "ILLEGAL MODE FOR THIS TRACK";
    if (a == 0x3a) return "MEDIUM NOT PRESENT";
    if (a == 0x30 && q == 0x05) return "CANNOT WRITE MEDIUM - INCOMPATIBLE FORMAT";
    if (a == 0x30 && q == 0x02) return "CANNOT READ MEDIUM - INCOMPATIBLE FORMAT";
    if (a == 0x30) return "INCOMPATIBLE MEDIUM INSTALLED";
    if (a == 0x21) return "LBA OUT OF RANGE";
    if (a == 0x00) return "no additional sense";
    return "?";
}

static void report(const char *label, int n)
{
    int k, a, q; kcq(&k, &a, &q);
    if (!st) { printf("   %-22s status=00  RETURNED %d bytes  <== ACCEPTED\n", label, n); return; }
    printf("   %-22s status=%02x sense=%d/%02x/%02x  %s\n", label, st, k, a, q, ascq_name(a, q));
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/sg3"; int allow_write = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--allow-write-probe")) allow_write = 1;
        else dev = argv[i];
    }
    fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror("open"); return 2; }
    unsigned char b[4096];
    int k, a, q;

    /* ---- what medium is this? ---- */
    printf("=== MEDIUM ===\n");
    int profile = -1;
    unsigned char gc[10] = { 0x46, 0x01, 0, 0, 0, 0, 0, 0, 32, 0 };
    if (cmd(gc, 10, b, 32) >= 8 && !st) profile = (b[6] << 8) | b[7];
    printf("   current profile     : 0x%04x", profile);
    switch (profile) {
        case 0x0000: printf("  (no medium / no profile)\n"); break;
        case 0x0008: printf("  CD-ROM (pressed, read-only)\n"); break;
        case 0x0009: printf("  CD-R\n"); break;
        case 0x000a: printf("  CD-RW\n"); break;
        case 0x0010: printf("  DVD-ROM\n"); break;
        case 0x0011: printf("  DVD-R\n"); break;
        case 0x0013: case 0x0014: printf("  DVD-RW\n"); break;
        case 0x001a: printf("  DVD+RW\n"); break;
        case 0x001b: printf("  DVD+R\n"); break;
        case 0x002b: printf("  DVD+R DL\n"); break;
        default: printf("\n");
    }
    unsigned char tur[6] = { 0,0,0,0,0,0 };
    cmd(tur, 6, NULL, 0); kcq(&k, &a, &q);
    int present = (st == 0) || (a != 0x3a);
    printf("   medium present      : %s\n", present ? "yes" : "NO (tray open / empty)");

    int erasable = -1, atip = -1;
    if (present) {
        unsigned char di[10] = { 0x51, 0,0,0,0,0,0, 0, 32, 0 };
        if (cmd(di, 10, b, 32) >= 3 && !st) erasable = (b[2] >> 4) & 1;
        unsigned char tc[10] = { 0x43, 0, 0x04, 0,0,0,0, 0, 28, 0 };
        int n = cmd(tc, 10, b, 28);
        atip = (!st && n > 4) ? 1 : 0;
        printf("   erasable bit        : %s\n", erasable < 0 ? "n/a" : (erasable ? "1 (RW)" : "0"));
        printf("   ATIP                : %s\n", atip ? "PRESENT (recordable)" : "absent (pressed)");
        /* track modes from the TOC control field */
        unsigned char t0[10] = { 0x43, 0, 0, 0,0,0, 0, 0, 40, 0 };
        n = cmd(t0, 10, b, 40);
        if (n >= 4 && !st) {
            printf("   tracks              :");
            for (int o = 4; o + 8 <= n; o += 8)
                printf(" %d=%s", b[o+2], (b[o+1] & 0x04) ? "DATA" : "audio");
            printf("\n");
        }
    }
    int writable = (atip == 1) || (erasable == 1) ||
                   (profile == 0x0009 || profile == 0x000a || profile == 0x0011 ||
                    profile == 0x0013 || profile == 0x0014 || profile == 0x0015 ||
                    profile == 0x001a || profile == 0x001b || profile == 0x002b);
    printf("   -> medium is %s\n", !present ? "ABSENT" : (writable ? "WRITABLE" : "unwritable"));

    /* ---- baseline probes ---- */
    printf("\n=== BASELINE (all-zero 12-byte CDB, 64-byte data-in) ===\n");
    int ops[3] = { 0xd9, 0xf2, 0xf4 };
    for (int i = 0; i < 3; i++) {
        char lbl[32]; snprintf(lbl, sizeof lbl, "0x%02X", ops[i]);
        if (present && writable && !allow_write) {
            printf("   %-22s SKIPPED — unknown opcode on a WRITABLE medium.\n", lbl);
            printf("   %-22s pass --allow-write-probe only if the disc is expendable.\n", "");
            continue;
        }
        unsigned char c[12] = { (unsigned char)ops[i], 0,0,0,0,0,0,0,0,0,0,0 };
        int n = cmd(c, 12, b, 64);
        report(lbl, n);
    }

    /* ---- field maps ---- */
    for (int i = 0; i < 3; i++) {
        if (present && writable && !allow_write) continue;
        printf("\n=== FIELD MAP 0x%02X (one CDB byte = 0x01) ===\n", ops[i]);
        for (int p = 1; p <= 10; p++) {
            unsigned char c[12] = { (unsigned char)ops[i], 0,0,0,0,0,0,0,0,0,0,0 };
            c[p] = 0x01;
            int n = cmd(c, 12, b, 64);
            char lbl[32]; snprintf(lbl, sizeof lbl, "byte %d = 01", p);
            report(lbl, n);
        }
    }
    close(fd);
    return 0;
}
