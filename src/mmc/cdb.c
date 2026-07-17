#include <string.h>

#include <accudisc/accudisc.h>

#include "cdb.h"

void adsc_cdb_inquiry(uint8_t cdb[6], uint8_t alloc)
{
    memset(cdb, 0, 6);
    cdb[0] = ADSC_OP_INQUIRY;
    cdb[4] = alloc;
}

void adsc_cdb_start_stop(uint8_t cdb[6], unsigned start, unsigned loej)
{
    memset(cdb, 0, 6);
    cdb[0] = ADSC_OP_START_STOP;
    cdb[4] = (uint8_t)(((loej & 1) << 1) | (start & 1));
}

void adsc_cdb_read_toc(uint8_t cdb[10], unsigned format, unsigned time_bit,
                       unsigned track, uint16_t alloc)
{
    memset(cdb, 0, 10);
    cdb[0] = ADSC_OP_READ_TOC;
    cdb[1] = (uint8_t)((time_bit & 1) << 1);
    cdb[2] = (uint8_t)(format & 0x0f);
    cdb[6] = (uint8_t)track;
    cdb[7] = (uint8_t)(alloc >> 8);
    cdb[8] = (uint8_t)(alloc & 0xff);
}

void adsc_cdb_read_disc_info(uint8_t cdb[10], uint16_t alloc)
{
    memset(cdb, 0, 10);
    cdb[0] = ADSC_OP_READ_DISC_INFO;
    cdb[1] = 0x00; /* data type: standard disc information */
    cdb[7] = (uint8_t)(alloc >> 8);
    cdb[8] = (uint8_t)(alloc & 0xff);
}

void adsc_cdb_write10(uint8_t cdb[10], uint32_t lba, uint16_t nblocks)
{
    memset(cdb, 0, 10);
    cdb[0] = ADSC_OP_WRITE10;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)(lba);
    cdb[7] = (uint8_t)(nblocks >> 8);
    cdb[8] = (uint8_t)(nblocks & 0xff);
}

void adsc_cdb_sync_cache(uint8_t cdb[10])
{
    memset(cdb, 0, 10);
    cdb[0] = ADSC_OP_SYNC_CACHE;
}

void adsc_cdb_send_cue(uint8_t cdb[10], uint32_t len)
{
    memset(cdb, 0, 10);
    cdb[0] = ADSC_OP_SEND_CUE;
    cdb[6] = (uint8_t)(len >> 16);
    cdb[7] = (uint8_t)(len >> 8);
    cdb[8] = (uint8_t)(len & 0xff);
}

void adsc_cdb_send_opc(uint8_t cdb[10])
{
    memset(cdb, 0, 10);
    cdb[0] = ADSC_OP_SEND_OPC;
    cdb[1] = 0x01; /* DoOPC: perform optimum power calibration */
}

void adsc_cdb_get_performance(uint8_t cdb[12], uint32_t start_lba,
                              uint16_t max_desc, unsigned type)
{
    memset(cdb, 0, 12);
    cdb[0] = ADSC_OP_GET_PERFORMANCE;
    /* byte 1 (data type) left 0: nominal, read, tolerance don't-care — the
     * value schily passes and the PX-716A returns its read curve for. */
    cdb[2] = (uint8_t)(start_lba >> 24);
    cdb[3] = (uint8_t)(start_lba >> 16);
    cdb[4] = (uint8_t)(start_lba >> 8);
    cdb[5] = (uint8_t)start_lba;
    cdb[8] = (uint8_t)(max_desc >> 8);   /* Maximum Number of Descriptors */
    cdb[9] = (uint8_t)(max_desc & 0xff);
    cdb[10] = (uint8_t)type;
}

void adsc_cdb_set_streaming(uint8_t cdb[12], uint16_t param_len)
{
    memset(cdb, 0, 12);
    cdb[0] = ADSC_OP_SET_STREAMING;
    /* Parameter List Length at bytes 9-10 (MSB first), NOT the usual Group-5
     * position (bytes 8-9). SET STREAMING is the one MMC command that shifts its
     * length field by a byte; schily cdrecord flags this "Sz not G5 alike"
     * (cdrecord/scsi_mmc.c:991). Hardware-verified on a PX-716A: length at 8-9
     * fails with 4/1b (SYNCHRONOUS DATA TRANSFER ERROR — the drive reads byte 9
     * as the length MSB, expects 0x1C00 = 7168 bytes, receives 28); moving it to
     * 9-10 makes the command succeed and the ceiling appears in mode page 2A. */
    cdb[9]  = (uint8_t)(param_len >> 8);
    cdb[10] = (uint8_t)(param_len & 0xff);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

void adsc_cdb_set_streaming_desc(uint8_t desc[28], unsigned speed_x,
                                 uint32_t start_lba, uint32_t end_lba)
{
    /* Descriptor layout:
     *   [0]      flags       [4..7]   Start LBA   [8..11]  End LBA
     *   [12..15] Read Size   [16..19] Read Time   [20..23] Write Size
     *   [24..27] Write Time
     * Rate = Read Size / Read Time; with Read Time = 1000 ms, Read Size is the
     * ceiling in kB/s. 1x CD-DA = 176.4 kB/s (7056 at 40x, matching the drive's
     * mode-page-2A max), so kB/s = speed_x * 1764 / 10.
     *
     * flags byte (MMC-5 SET STREAMING performance descriptor, header 0):
     *   bit0 RA (0x01), bit1 Exact (0x02), bit2 RDD (0x04), bits3-4 WRC, 5-7 rsvd.
     * Normal ceiling: all flags clear (0x00). Exact stays 0 so the drive is free
     * to run CAV under the ceiling rather than pinning the exact rate (CLV).
     * speed_x == 0 restores defaults via the real RDD bit (0x04).
     * (An earlier version wrote 0x40/0x20 — both reserved bits: the ceiling path
     * worked only because the drive ignored 0x40, and RDD never fired at all.) */
    uint32_t read_size = (uint32_t)speed_x * 1764u / 10u;

    memset(desc, 0, 28);
    if (speed_x == 0) {
        desc[0] = 0x04; /* RDD: restore drive defaults */
    } else {
        desc[0] = 0x00; /* ceiling, Exact clear -> drive may run CAV underneath */
        put_be32(desc + 12, read_size); /* Read Size (kB) */
        put_be32(desc + 16, 1000);      /* Read Time (ms) */
        put_be32(desc + 20, read_size); /* Write Size (mirror; unused for read) */
        put_be32(desc + 24, 1000);      /* Write Time (ms) */
    }
    put_be32(desc + 4, start_lba);
    put_be32(desc + 8, end_lba);
}

void adsc_cdb_get_configuration(uint8_t cdb[10], unsigned rt,
                                uint16_t feature, uint16_t alloc)
{
    memset(cdb, 0, 10);
    cdb[0] = ADSC_OP_GET_CONFIG;
    cdb[1] = (uint8_t)(rt & 0x03);
    cdb[2] = (uint8_t)(feature >> 8);
    cdb[3] = (uint8_t)(feature & 0xff);
    cdb[7] = (uint8_t)(alloc >> 8);
    cdb[8] = (uint8_t)(alloc & 0xff);
}

void adsc_cdb_mode_sense10(uint8_t cdb[10], unsigned page, uint16_t alloc)
{
    memset(cdb, 0, 10);
    cdb[0] = ADSC_OP_MODE_SENSE10;
    cdb[2] = (uint8_t)(page & 0x3f);
    cdb[7] = (uint8_t)(alloc >> 8);
    cdb[8] = (uint8_t)(alloc & 0xff);
}

void adsc_cdb_mode_select10(uint8_t cdb[10], uint16_t param_len)
{
    memset(cdb, 0, 10);
    cdb[0] = ADSC_OP_MODE_SELECT10;
    cdb[1] = 0x10; /* PF = SPC-format pages */
    cdb[7] = (uint8_t)(param_len >> 8);
    cdb[8] = (uint8_t)(param_len & 0xff);
}

void adsc_cdb_read_cd(uint8_t cdb[12], uint32_t lba, uint32_t nsec,
                      unsigned sector_type, unsigned c2, unsigned sub)
{
    memset(cdb, 0, 12);
    cdb[0] = ADSC_OP_READ_CD;
    cdb[1] = (uint8_t)((sector_type & 0x07) << 2);
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[6] = (uint8_t)(nsec >> 16);
    cdb[7] = (uint8_t)(nsec >> 8);
    cdb[8] = (uint8_t)nsec;
    cdb[9] = (uint8_t)(0x10 | ((c2 & 0x03) << 1)); /* user data | error flags */
    cdb[10] = (uint8_t)(sub & 0x07);
}

uint32_t adsc_read_cd_sector_len(unsigned c2, unsigned sub)
{
    uint32_t len = ACCUDISC_BYTES_AUDIO;

    if (c2 == ADSC_C2_294)
        len += ACCUDISC_BYTES_C2;
    else if (c2 == ADSC_C2_296)
        len += ACCUDISC_BYTES_C2_BEB;
    if (sub == ADSC_SUB_RAW)
        len += ACCUDISC_BYTES_SUB_RAW;
    else if (sub == ADSC_SUB_Q)
        len += ACCUDISC_BYTES_SUB_Q;
    return len;
}
