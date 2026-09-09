/* selsweep — sweep one vendor opcode's selector byte, GET forms only.
 * (vendor-driver zone; not built or shipped)
 *
 * SELECTOR SWEEP Phase 2. Gated on Phase 0 having established a discriminator:
 * this tool is only meaningful where an ABSENT selector is distinguishable from
 * an implemented one that happens to read zero. On the PX-716A both 0xE9 and
 * 0xED answer an absent selector with CHECK CONDITION 5/24/00 INVALID FIELD IN
 * CDB, which is the discriminator this tool keys on.
 *
 * GET forms only. CDB[1] is held at 0x00 throughout — on BOTH opcodes a SET is
 * one bit away in that byte, and this tool must never issue one.
 *
 *   0xE9  e9 00 <page> 00 00 00 00 00 00 00 08 00   data-in 8, len at CDB[10]
 *   0xED  ed 00 <mode> 00 00 00 00 00 00 08 00 00   data-in 8, len at CDB[8..9]
 *
 * Every CDB is traced to stderr, unbuffered, BEFORE it is issued.
 *
 *   build: gcc -O2 -Wall -o selsweep selsweep.c
 *   run:   flock /var/tmp/sr0.lock ./selsweep --dev /dev/sg3 --op e9
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>

int main(int argc, char **argv)
{
    const char *dev = "/dev/sg3";
    int op = 0xe9;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dev") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "--op") && i + 1 < argc)
            op = (int)strtol(argv[++i], 0, 16);
        else { fprintf(stderr, "usage: selsweep --dev /dev/sgN --op e9|ed\n"); return 2; }
    }
    if (op != 0xe9 && op != 0xed) {
        fprintf(stderr, "selsweep: only 0xE9 and 0xED have a Phase 0 discriminator\n");
        return 2;
    }

    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror(dev); return 1; }

    printf("== 0x%02X selector sweep, GET only, 0x00-0xFF ==\n", op);
    printf("   sel  status  sense      data\n");

    int nimpl = 0, nabsent = 0, nother = 0;
    unsigned char impl[256];

    for (int sel = 0; sel <= 0xff; sel++) {
        unsigned char cdb[12] = {0}, data[8], sense[64];
        cdb[0] = (unsigned char)op;
        cdb[1] = 0x00;                       /* GET. Never 0x10 / 0x08. */
        cdb[2] = (unsigned char)sel;
        if (op == 0xe9) cdb[10] = 8;
        else            cdb[9]  = 8;

        fprintf(stderr, "CDB:");
        for (int i = 0; i < 12; i++) fprintf(stderr, " %02x", cdb[i]);
        fprintf(stderr, "\n");
        fflush(stderr);

        memset(data, 0, sizeof data);
        memset(sense, 0, sizeof sense);
        sg_io_hdr_t io;
        memset(&io, 0, sizeof io);
        io.interface_id = 'S';
        io.dxfer_direction = SG_DXFER_FROM_DEV;
        io.cmd_len = 12;
        io.cmdp = cdb;
        io.dxferp = data;
        io.dxfer_len = sizeof data;
        io.sbp = sense;
        io.mx_sb_len = sizeof sense;
        io.timeout = 60000;                  /* a timeout is NOT a hang */

        if (ioctl(fd, SG_IO, &io) < 0) {
            printf("   0x%02x  IOCTL FAILED (%s)\n", sel, "ioctl");
            nother++;
            continue;
        }

        int key = sense[2] & 0x0f, asc = sense[12], ascq = sense[13];

        /* io.status is the SCSI status ONLY. A transport failure shows up in
         * host_status (DID_ERROR == 7) with io.status still 0, so testing
         * io.status alone reports a failed command as GOOD. The first version
         * of this tool did exactly that and produced three false IMPLEMENTED
         * rows, whose "data" was the previous successful response still sitting
         * in the buffer. Both must be clear, and the transfer must have
         * actually happened (resid == 0), before a response is believed.
         *
         * The third class is labelled FAILS DIFFERENTLY, not "recognised":
         * failing unlike an unassigned selector does not establish that the
         * firmware knows the value, since a handler-less selector in a valid
         * range could fall over the same way. 0x41 was later shown to be
         * recognised by a DIFFERENT observation - a medium-aware sense code
         * with the tray open - and 0x06/0x07 still have no such evidence. */
        if (io.status == 0 && io.host_status == 0 && io.driver_status == 0
            && io.resid == 0) {
            printf("   0x%02x  GOOD    -          ", sel);
            for (int i = 0; i < 8; i++) printf("%02x ", data[i]);
            printf(" <== IMPLEMENTED\n");
            impl[nimpl++] = (unsigned char)sel;
        } else if (io.host_status == 0 && key == 0x05 && asc == 0x24
                   && ascq == 0x00) {
            nabsent++;                       /* the discriminator: unassigned */
        } else {
            printf("   0x%02x  st=%02x host=%02x  %x/%02x/%02x  resid=%d"
                   "  <== FAILS DIFFERENTLY\n",
                   sel, io.status, io.host_status, key, asc, ascq, io.resid);
            nother++;
        }
    }

    printf("   -> %d implemented, %d absent (5/24/00), %d other\n",
           nimpl, nabsent, nother);
    printf("   implemented:");
    for (int i = 0; i < nimpl; i++) printf(" 0x%02x", impl[i]);
    printf("\n");
    close(fd);
    return 0;
}
