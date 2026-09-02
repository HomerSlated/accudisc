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
    adsc_cdb_set_streaming_desc(desc, 40, 0, 0xFFFFFFFFu, 0, 1411);
    const uint8_t want40[28] = {
        0x00, 0x00, 0x00, 0x00,             /* flags (Exact clear -> CAV ok) */
        0x00, 0x00, 0x00, 0x00,             /* start LBA 0 */
        0xFF, 0xFF, 0xFF, 0xFF,             /* end LBA = whole disc */
        0x00, 0x00, 0x1B, 0x90,             /* read size 7056 */
        0x00, 0x00, 0x03, 0xE8,             /* read time 1000 */
        0x00, 0x00, 0x05, 0x83,             /* write size 1411 = what was PASSED,
                                             * NOT the 7056 read rate. Mirroring
                                             * the read rate here is the leak
                                             * fixed in 0.31.0: the drive obeys
                                             * this field, so a read-speed
                                             * change retuned writing. */
        0x00, 0x00, 0x03, 0xE8,             /* write time 1000 */
    };
    assert(memcmp(desc, want40, 28) == 0);

    /* 48x (SpeedRead rung) => 8467 kB/s = 0x2113. */
    adsc_cdb_set_streaming_desc(desc, 48, 0, 0xFFFFFFFFu, 0, 1411);
    assert(desc[12] == 0x00 && desc[13] == 0x00 &&
           desc[14] == 0x21 && desc[15] == 0x13);

    /* exact != 0 sets the Exact bit (0x02) = pin the rate / force CLV. */
    adsc_cdb_set_streaming_desc(desc, 8, 0, 0xFFFFFFFFu, 1, 1411);
    assert(desc[0] == 0x02);

    /* LBA-scoped: a slow 8x pass over a damaged span [1000, 2000). */
    adsc_cdb_set_streaming_desc(desc, 8, 1000, 2000, 0, 1411);
    assert(desc[0] == 0x00);
    assert(desc[4] == 0x00 && desc[5] == 0x00 &&
           desc[6] == 0x03 && desc[7] == 0xE8);   /* start 1000 */
    assert(desc[8] == 0x00 && desc[9] == 0x00 &&
           desc[10] == 0x07 && desc[11] == 0xD0); /* end 2000 */
    assert(desc[14] == 0x05 && desc[15] == 0x83); /* 8 * 1764 / 10 = 1411 = 0x0583 */

    /* speed 0 => restore defaults: real RDD flag (0x04, bit2), zero rate.
     * RDD wins even if exact is requested — and it zeroes the write fields too,
     * so write_kbps is deliberately ignored on this path. */
    adsc_cdb_set_streaming_desc(desc, 0, 0, 0xFFFFFFFFu, 1, 1411);
    assert(desc[0] == 0x04);
    for (int i = 12; i < 28; i++)
        assert(desc[i] == 0x00);

    /* THE 0.31.0 LEAK, pinned in both directions.
     *
     * Write Size must be the value PASSED, never the read rate. Mirroring the
     * read rate is what made `accudisc speed N` — a read-speed command —
     * retune the write speed at every N; and it did so past a ceiling the
     * caller had left in place (SpeedRead off, max 40x: the read field clamped
     * to 40x and the write field went to 48x).
     *
     * Distinct values throughout so a mirror cannot pass by coincidence:
     * read 32x = 5644, write asked 1411 (8x). */
    adsc_cdb_set_streaming_desc(desc, 32, 0, 0xFFFFFFFFu, 0, 1411);
    assert(desc[14] == 0x16 && desc[15] == 0x0C);   /* read  5644 */
    assert(desc[22] == 0x05 && desc[23] == 0x83);   /* write 1411, NOT 5644 */

    /* ...and 0 is NOT a safe "leave alone": the drive reads it as MAXIMUM
     * (measured, PX-716A: Write Size 0 sent the write speed to 48x). So a
     * caller that has no write speed to report gets the old behaviour, which
     * is the honest floor rather than a silent improvement. */
    adsc_cdb_set_streaming_desc(desc, 32, 0, 0xFFFFFFFFu, 0, 0);
    assert(desc[20] == 0 && desc[21] == 0 && desc[22] == 0 && desc[23] == 0);

    /* WHAT THIS CANNOT REACH, so nobody reads it as full cover. These pin the
     * DESCRIPTOR. They say nothing about accudisc_set_speed actually READING
     * the current write speed before it builds one — deleting that call leaves
     * the whole suite green (mutation-tested 2026-08-28), because no
     * device-free test can observe a page-2A round trip.
     *
     * That half is verified on hardware instead, PX-716A, and the result is in
     * TODO.md: from READ 24x / WRITE 8x, `accudisc speed 32` left the write
     * speed at 8x, and `speed 48` with the uncap off quantized the read to 40x
     * while still leaving the write speed at 8x — the case that used to escape
     * the ceiling entirely. */
}

/* The six write-path CDBs. Added 2026-09-02 to close OPCODES.md §G item 2:
 * tests/test_burn_flow.c stubs adsc_mmc_write10, adsc_mmc_send_opc,
 * adsc_mmc_sync_cache, adsc_mmc_send_cue_sheet, adsc_mmc_set_cd_speed and
 * adsc_mmc_read_buffer_capacity, so
 * it asserts the burn SEQUENCE and would pass identically with garbage CDB
 * headers. Its write10 stub discards its arguments outright
 * ((void)lba; (void)nblocks;), which means no hardware-free test had ever
 * observed which LBA the burn writes to.
 *
 * Three of the five were only half-uncovered and the distinction is worth
 * keeping: 0x5D's PAYLOAD is asserted byte-by-byte in test_cuesheet.c, and
 * 0xBB's Nx->kB/s conversion is covered because cdb.c is linked for real into
 * test_burn_flow. Neither of those touches the CDB header. 0x2A and 0x54 were
 * bare in both directions. */
static void test_write_path_cdbs(void)
{
    uint8_t cdb[12];

    /* --- 0x2A WRITE(10) ------------------------------------------------ */

    /* THE LEAD-IN ADDRESS, which is the whole reason this test exists.
     *
     * burn.c:536 starts the lead-in gap at `int32_t lba = -(int32_t)LEADIN_GAP`
     * = -150, and mmc.c:186 casts it to uint32_t on the way in. So the first
     * WRITE(10) of every burn carries the two's-complement of -150,
     * 0xFFFFFF6A, big-endian at bytes 2-5. Nothing else in the tree addresses a
     * negative LBA, and nothing device-free had ever looked at this value.
     *
     * A refactor of adsc_cdb_write10's parameter to int32_t, or of the wrapper
     * to reject "out of range" LBAs, would break the lead-in and leave the rest
     * of the suite green. */
    adsc_cdb_write10(cdb, (uint32_t)(int32_t)-150, 27);
    const uint8_t want_leadin[10] = {0x2A, 0x00, 0xFF, 0xFF, 0xFF,
                                     0x6A, 0x00, 0x00, 0x1B, 0x00};
    assert(memcmp(cdb, want_leadin, 10) == 0);

    /* The lead-in walk crosses zero on its last chunk. 150 = 5*27 + 15, so
     * burn.c writes CHUNK sectors five times and a 15-sector remainder at LBA
     * -15 (0xFFFFFFF1) that lands exactly on 0. Pinned because "the LBA is
     * negative" and "the LBA is negative and the count is short" are different
     * arithmetic, and only the second one ends the gap in the right place. */
    adsc_cdb_write10(cdb, (uint32_t)(int32_t)-15, 15);
    assert(cdb[2] == 0xFF && cdb[3] == 0xFF && cdb[4] == 0xFF && cdb[5] == 0xF1);
    assert(cdb[7] == 0x00 && cdb[8] == 0x0F);

    /* Track audio: LBA 0 onward, CHUNK = 27 sectors (27*2352 = 63504 B). */
    adsc_cdb_write10(cdb, 0, 27);
    const uint8_t want_audio[10] = {0x2A, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x1B, 0x00};
    assert(memcmp(cdb, want_audio, 10) == 0);

    /* Byte 1 stays zero: no FUA, no FUA_NV, no protection. Byte 6 (Group
     * Number) and byte 9 (Control) likewise. A drive that honoured a stray FUA
     * on an audio burn would be flushing per chunk. */
    adsc_cdb_write10(cdb, 0x00123456, 1);
    assert(cdb[1] == 0x00 && cdb[6] == 0x00 && cdb[9] == 0x00);
    assert(cdb[2] == 0x00 && cdb[3] == 0x12 && cdb[4] == 0x34 && cdb[5] == 0x56);

    /* Transfer length is 16-bit at bytes 7-8, big-endian, and the maximum is
     * representable. CHUNK is 27 so this is headroom, not a live path.
     *
     * The builder is not where the hazard was. adsc_mmc_write10 takes nblocks
     * as uint32_t and casts to uint16_t for the CDB while sizing the buffer
     * from the UNTRUNCATED value — so at nblocks == 65536 the CDB would say
     * zero blocks and cmd.buf_len would say 154 MB, both well-formed, nothing
     * downstream able to reject either. Unreachable at CHUNK == 27, but a test
     * comment is the wrong place to keep that record, so the wrapper now
     * rejects it outright (mmc.c, alongside the nblocks == 0 guard). */
    adsc_cdb_write10(cdb, 0, 0xFFFF);
    assert(cdb[7] == 0xFF && cdb[8] == 0xFF);

    /* --- 0x5C READ BUFFER CAPACITY ------------------------------------- */

    /* The sixth stub in test_burn_flow.c, issued on every burn. Block (byte 1
     * bit 0) stays CLEAR so the drive reports in BYTES. This is the one field
     * that matters and the one that cannot be caught downstream: cdb.h records
     * that on an idle drive — where blank == length — the byte reading and the
     * block reading are INDISTINGUISHABLE, so the "it matches" check that would
     * normally settle it proves only that the buffer is empty. Allocation
     * length 12 = the whole Buffer Capacity structure; a shorter one truncates
     * the second field silently. */
    adsc_cdb_read_buffer_capacity(cdb);
    const uint8_t want_bufcap[10] = {0x5C, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x0C, 0x00};
    assert(memcmp(cdb, want_bufcap, 10) == 0);

    /* --- 0x54 SEND OPC INFORMATION ------------------------------------- */

    /* DoOPC (byte 1 bit 0) MUST be set. This is the command's entire content:
     * with the bit clear the drive returns whatever OPC data it already holds
     * and calibrates NOTHING, succeeding either way. That is a silent no-op on
     * the write path's power calibration — well-formed enough that nothing
     * downstream could reject it. */
    adsc_cdb_send_opc(cdb);
    const uint8_t want_opc[10] = {0x54, 0x01, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0x00};
    assert(memcmp(cdb, want_opc, 10) == 0);

    /* --- 0x35 SYNCHRONIZE CACHE ---------------------------------------- */

    /* Immed (byte 1 bit 1) stays CLEAR, so the command does not return until
     * the drive has flushed. burn.c relies on that: 0x35 is both the close and
     * the whole abort path here, and an immediate-return flush would report
     * success before the disc was finished. All-zero after the opcode also
     * means LBA 0 / length 0 = "the whole cache", not a range. */
    adsc_cdb_sync_cache(cdb);
    const uint8_t want_sync[10] = {0x35, 0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00, 0x00, 0x00};
    assert(memcmp(cdb, want_sync, 10) == 0);

    /* --- 0x5D SEND CUE SHEET ------------------------------------------- */

    /* The length field is 24-bit at bytes 6-8, NOT the usual 16-bit Group-5
     * slot at 7-8. Cue sheets are 8 bytes per entry, so a 16-bit field would
     * hold 8191 entries and never overflow in practice — which is exactly why
     * a wrong layout here would go unnoticed: put the length at 7-8 and byte 6
     * reads as zero, so short sheets still work and the failure only appears
     * past 64 KB. Pin all three bytes. */
    adsc_cdb_send_cue(cdb, 312);            /* 39 entries: a plausible album */
    const uint8_t want_cue[10] = {0x5D, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x01, 0x38, 0x00};
    assert(memcmp(cdb, want_cue, 10) == 0);

    /* A value that cannot fit in 16 bits, so the top byte has to carry it. */
    adsc_cdb_send_cue(cdb, 0x012345);
    assert(cdb[6] == 0x01 && cdb[7] == 0x23 && cdb[8] == 0x45);

    /* --- 0xBB SET CD SPEED --------------------------------------------- */

    /* 12-byte CDB. Read speed at 2-3, write speed at 4-5, rotational control in
     * byte 1 — verified against cdrecord scsi_cdr.c:520 (see cdb.c). Speeds are
     * kB/s, never Nx; adsc_cd_speed_kbps does the conversion and is covered by
     * test_burn_flow through the real cdb.c, but the CDB it feeds was not. */
    adsc_cdb_set_cd_speed(cdb, adsc_cd_speed_kbps(8), adsc_cd_speed_kbps(4),
                          ADSC_ROTCTL_CLV);
    const uint8_t want_speed[12] = {0xBB, 0x00, 0x05, 0x88, 0x02, 0xC4,
                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    assert(memcmp(cdb, want_speed, 12) == 0);   /* 8*177=1416, 4*177=708 */

    /* CLV is what a CD write wants; CAV is a ceiling, not a rate. Distinct
     * read/write values so a builder that mirrored one into the other — the
     * 0.31.0 leak, in its SET STREAMING form — could not pass here either. */
    adsc_cdb_set_cd_speed(cdb, 0x1B90, 0x0583, ADSC_ROTCTL_CAV);
    assert(cdb[1] == 0x01);
    assert(cdb[2] == 0x1B && cdb[3] == 0x90);
    assert(cdb[4] == 0x05 && cdb[5] == 0x83);

    /* 0xFFFF is "leave alone / maximum" in either field and must pass through
     * intact rather than being clamped or zeroed. */
    adsc_cdb_set_cd_speed(cdb, 0xFFFF, 0xFFFF, ADSC_ROTCTL_CLV);
    assert(cdb[2] == 0xFF && cdb[3] == 0xFF);
    assert(cdb[4] == 0xFF && cdb[5] == 0xFF);

    /* WHAT THIS CANNOT REACH, so nobody reads it as cover for the burn.
     *
     * These pin five CDB layouts. They say nothing about the burn ISSUING them
     * in the right order, with the right arguments, at the right moment — that
     * is test_burn_flow's job, and its stubs are why the two halves have to be
     * read together. In particular nothing here observes that burn.c walks the
     * lead-in from -150 to 0 at all; delete the loop at burn.c:536-543 and this
     * file stays green. */
}

int main(void)
{
    test_read_cd();
    test_sector_len();
    test_read_toc();
    test_misc();
    test_get_performance();
    test_set_streaming();
    test_write_path_cdbs();
    return 0;
}
