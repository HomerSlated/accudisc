/* CDB builder layout tests. Expected bytes derived from c2read.c, whose
 * layouts were pinned from redumper scsi/mmc.ixx and validated on hardware. */

#include <assert.h>
#include <string.h>

#include "mmc/cdb.h"

static void test_read_cd(void)
{
    uint8_t cdb[12];

    /* CD-DA, C2 pointers, no sub — the c2read default read. */
    adsc_cdb_read_cd(cdb, 0x123456, 24, ADSC_SECTOR_CDDA, ADSC_C2_294,
                     ADSC_SUB_NONE);
    const uint8_t want[12] = {0xBE, 0x04, 0x00, 0x12, 0x34, 0x56,
                              0x00, 0x00, 0x18, 0x12, 0x00, 0x00};
    assert(memcmp(cdb, want, 12) == 0);

    /* ALL types, C2+BEB, raw P-W sub. */
    adsc_cdb_read_cd(cdb, 0, 1, ADSC_SECTOR_ANY, ADSC_C2_296, ADSC_SUB_RAW);
    assert(cdb[1] == 0x00);
    assert(cdb[9] == 0x14);  /* user data | c2mode 2 << 1 */
    assert(cdb[10] == 0x01);

    /* Formatted Q sub, no C2. */
    adsc_cdb_read_cd(cdb, 0, 1, ADSC_SECTOR_CDDA, ADSC_C2_NONE, ADSC_SUB_Q);
    assert(cdb[9] == 0x10);
    assert(cdb[10] == 0x02);
}

static void test_sector_len(void)
{
    assert(adsc_read_cd_sector_len(ADSC_C2_NONE, ADSC_SUB_NONE) == 2352);
    assert(adsc_read_cd_sector_len(ADSC_C2_294, ADSC_SUB_NONE) == 2352 + 294);
    assert(adsc_read_cd_sector_len(ADSC_C2_296, ADSC_SUB_NONE) == 2352 + 296);
    assert(adsc_read_cd_sector_len(ADSC_C2_294, ADSC_SUB_RAW) ==
           2352 + 294 + 96);
    assert(adsc_read_cd_sector_len(ADSC_C2_294, ADSC_SUB_Q) ==
           2352 + 294 + 16);
}

static void test_read_toc(void)
{
    uint8_t cdb[10];

    /* Format 0, LBA, from track 1 — the lead-out lookup read. */
    adsc_cdb_read_toc(cdb, ADSC_TOC_FMT_TOC, 0, 1, 1024);
    const uint8_t want[10] = {0x43, 0x00, 0x00, 0x00, 0x00,
                              0x00, 0x01, 0x04, 0x00, 0x00};
    assert(memcmp(cdb, want, 10) == 0);

    /* Full TOC: format 2, time=1, session 1 (c2read --fulltoc). */
    adsc_cdb_read_toc(cdb, ADSC_TOC_FMT_FULL, 1, 1, 4);
    assert(cdb[1] == 0x02);
    assert(cdb[2] == 0x02);
    assert(cdb[6] == 0x01);
    assert(cdb[7] == 0x00 && cdb[8] == 0x04);

    /* CD-Text: format 5, time=0, track 0 (c2read --cdtext). */
    adsc_cdb_read_toc(cdb, ADSC_TOC_FMT_CDTEXT, 0, 0, 0xffff);
    assert(cdb[1] == 0x00);
    assert(cdb[2] == 0x05);
    assert(cdb[7] == 0xff && cdb[8] == 0xff);
}

static void test_misc(void)
{
    uint8_t cdb6[6];
    uint8_t cdb10[10];

    adsc_cdb_inquiry(cdb6, 36);
    assert(cdb6[0] == 0x12 && cdb6[4] == 36 && cdb6[5] == 0);

    /* Spindle down, no eject (c2read --stop). */
    adsc_cdb_start_stop(cdb6, 0, 0);
    assert(cdb6[0] == 0x1B && cdb6[4] == 0x00);
    adsc_cdb_start_stop(cdb6, 1, 0);
    assert(cdb6[4] == 0x01);
    adsc_cdb_start_stop(cdb6, 0, 1);
    assert(cdb6[4] == 0x02);

    /* CD Read feature 0x001E, RT=10b (c2read --features). */
    adsc_cdb_get_configuration(cdb10, 0x02, 0x001E, 64);
    assert(cdb10[0] == 0x46 && cdb10[1] == 0x02);
    assert(cdb10[2] == 0x00 && cdb10[3] == 0x1E);
    assert(cdb10[7] == 0x00 && cdb10[8] == 64);

    adsc_cdb_mode_sense10(cdb10, 0x2a, 256);
    assert(cdb10[0] == 0x5A && cdb10[2] == 0x2a);
    assert(cdb10[7] == 0x01 && cdb10[8] == 0x00);

    adsc_cdb_mode_select10(cdb10, 0x30);
    assert(cdb10[0] == 0x55 && cdb10[1] == 0x10);
    assert(cdb10[7] == 0x00 && cdb10[8] == 0x30);
}

static void test_get_performance(void)
{
    uint8_t cdb[12];

    /* Nominal curve from LBA 0, up to 16 descriptors. Max-descriptor count at
     * bytes 8-9 (normal Group-5 slot, unlike SET STREAMING), type at byte 10. */
    adsc_cdb_get_performance(cdb, 0, 16, ADSC_PERF_TYPE_NOMINAL);
    assert(cdb[0] == 0xAC);
    assert(cdb[1] == 0x00);                    /* data type: nominal read */
    assert(cdb[2] == 0 && cdb[3] == 0 && cdb[4] == 0 && cdb[5] == 0);
    assert(cdb[8] == 0x00 && cdb[9] == 0x10);  /* 16 descriptors */
    assert(cdb[10] == 0x00);                   /* type 0 = performance data */
    assert(cdb[11] == 0x00);

    /* Start LBA is big-endian at bytes 2-5. */
    adsc_cdb_get_performance(cdb, 0x00123456, 1, ADSC_PERF_TYPE_NOMINAL);
    assert(cdb[2] == 0x00 && cdb[3] == 0x12 && cdb[4] == 0x34 && cdb[5] == 0x56);
    assert(cdb[8] == 0x00 && cdb[9] == 0x01);
}

static void test_set_streaming(void)
{
    uint8_t cdb[12];
    uint8_t desc[28];

    /* CDB: opcode + param list length 28 (0x001C) at bytes 9-10 (NOT the usual
     * Group-5 8-9 slot; schily "Sz not G5 alike", hardware-verified — see
     * cdb.c). Placing it at 8-9 makes the PX-716A reject with 4/1b. */
    adsc_cdb_set_streaming(cdb, 28);
    assert(cdb[0] == 0xB6);
    assert(cdb[9] == 0x00 && cdb[10] == 0x1C);
    for (int i = 1; i <= 8; i++)
        assert(cdb[i] == 0x00);
    assert(cdb[11] == 0x00);

    /* 40x whole-disc ceiling: flags 0x00 (all clear: RA/Exact/RDD=0), start 0,
     * end all-FF, Read Size 7056 kB/s (= 40 * 176.4, the drive's page-2A max),
     * Read Time 1000 ms. */
    adsc_cdb_set_streaming_desc(desc, 40, 0, 0xFFFFFFFFu, 0);
    const uint8_t want40[28] = {
        0x00, 0x00, 0x00, 0x00,             /* flags (Exact clear -> CAV ok) */
        0x00, 0x00, 0x00, 0x00,             /* start LBA 0 */
        0xFF, 0xFF, 0xFF, 0xFF,             /* end LBA = whole disc */
        0x00, 0x00, 0x1B, 0x90,             /* read size 7056 */
        0x00, 0x00, 0x03, 0xE8,             /* read time 1000 */
        0x00, 0x00, 0x1B, 0x90,             /* write size 7056 */
        0x00, 0x00, 0x03, 0xE8,             /* write time 1000 */
    };
    assert(memcmp(desc, want40, 28) == 0);

    /* 48x (SpeedRead rung) => 8467 kB/s = 0x2113. */
    adsc_cdb_set_streaming_desc(desc, 48, 0, 0xFFFFFFFFu, 0);
    assert(desc[12] == 0x00 && desc[13] == 0x00 &&
           desc[14] == 0x21 && desc[15] == 0x13);

    /* exact != 0 sets the Exact bit (0x02) = pin the rate / force CLV. */
    adsc_cdb_set_streaming_desc(desc, 8, 0, 0xFFFFFFFFu, 1);
    assert(desc[0] == 0x02);

    /* LBA-scoped: a slow 8x pass over a damaged span [1000, 2000). */
    adsc_cdb_set_streaming_desc(desc, 8, 1000, 2000, 0);
    assert(desc[0] == 0x00);
    assert(desc[4] == 0x00 && desc[5] == 0x00 &&
           desc[6] == 0x03 && desc[7] == 0xE8);   /* start 1000 */
    assert(desc[8] == 0x00 && desc[9] == 0x00 &&
           desc[10] == 0x07 && desc[11] == 0xD0); /* end 2000 */
    assert(desc[14] == 0x05 && desc[15] == 0x83); /* 8 * 1764 / 10 = 1411 = 0x0583 */

    /* speed 0 => restore defaults: real RDD flag (0x04, bit2), zero rate.
     * RDD wins even if exact is requested. */
    adsc_cdb_set_streaming_desc(desc, 0, 0, 0xFFFFFFFFu, 1);
    assert(desc[0] == 0x04);
    for (int i = 12; i < 28; i++)
        assert(desc[i] == 0x00);
}

int main(void)
{
    test_read_cd();
    test_sector_len();
    test_read_toc();
    test_misc();
    test_get_performance();
    test_set_streaming();
    return 0;
}
