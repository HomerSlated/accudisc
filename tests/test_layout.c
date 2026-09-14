/* READ CD record layout (0.39.0): the sub-before-C2 order and the 16-byte
 * transfer rounding, against a fake drive that behaves the way a LITE-ON
 * LH-20A1S 9L08 on libata was measured to behave on 2026-09-14.
 *
 * The fake has to be able to be wrong in the ways the drive was, or it proves
 * nothing:
 *   - DMA (transfer a multiple of 16): records at the requested stride, in the
 *     drive's order, and the rest of dxfer_len overwritten with zeros — the
 *     kernel hands back the whole transfer, which is what made a sentinel
 *     useless for measuring length and what makes rounding against the
 *     caller's own buffer an overrun;
 *   - PIO (not a multiple of 16): every record padded to 16 bytes with `00 00`
 *     plus the previous record's audio bytes 696-703, truncated at dxfer_len.
 *
 * Audio is a position-derived pattern, and every Q frame carries its own LBA
 * with a valid CRC, so a displaced, reordered or wrongly-sliced record cannot
 * pass by accident. adsc_dev_exec is replaced with -Wl,--wrap. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "mmc/mmc.h"
#include "cdda/crc16.h"
#include "cdda/layout.h"

#define AUDIO 2352u
#define SUB 96u
#define C2 294u
#define REC (AUDIO + C2 + SUB)

/* ---- disc content -------------------------------------------------------- */

static uint8_t audio_byte(uint32_t lba, uint32_t i)
{
    uint32_t v = (lba * 2654435761u) ^ (i * 40503u) ^ 0x9E3779B9u;

    return (uint8_t)(v >> ((i & 3u) * 8u));
}

static uint8_t to_bcd(unsigned v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

/* Raw interleaved P-W for one sector: Q at bit 6 carries ADR 1 with the
 * absolute MSF of `lba` and a valid CRC; R-W are all ones, P is zero — the
 * pattern the drive actually returned. `broken` flips a Q bit. */
static void make_sub(uint32_t lba, int broken, uint8_t raw[96])
{
    uint8_t q[12] = {0};
    uint32_t a = lba + 150;

    q[0] = 0x01; q[1] = 0x01; q[2] = 0x01;
    q[7] = to_bcd(a / 4500); q[8] = to_bcd((a / 75) % 60); q[9] = to_bcd(a % 75);
    uint16_t crc = (uint16_t)~adsc_crc16(q, 10);
    q[10] = (uint8_t)(crc >> 8); q[11] = (uint8_t)crc;
    if (broken)
        q[4] ^= 0x10;
    for (unsigned i = 0; i < 96; i++)
        raw[i] = (uint8_t)(0x3F | (((q[i >> 3] >> (7 - (i & 7))) & 1) << 6));
}

/* One record in the MMC order, the reference every result is compared with. */
static void make_record_mmc(uint32_t lba, uint8_t *rec)
{
    for (uint32_t i = 0; i < AUDIO; i++)
        rec[i] = audio_byte(lba, i);
    memset(rec + AUDIO, 0, C2);
    make_sub(lba, 0, rec + AUDIO + C2);
}

/* ---- the fake drive ------------------------------------------------------ */

static struct {
    int sub_first;          /* the LITE-ON order */
    int zero_sub;           /* P-W zero-filled: no evidence at either position */
    int reject_rounded;     /* transport fails any transfer > the data */
    uint32_t c2_hot_lba;    /* this LBA's C2 field carries one fired bit */
    unsigned reads;
    uint32_t last_xfer;
    void *last_buf;
} fk;

int __real_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd);
int __wrap_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd)
{
    (void)dev;
    cmd->resid = 0;
    if (cmd->cdb[0] != 0xBE) {
        if (cmd->dir == ADSC_XFER_IN && cmd->buf)
            memset(cmd->buf, 0, cmd->buf_len);
        return ACCUDISC_OK;
    }

    uint32_t lba = (uint32_t)cmd->cdb[2] << 24 | (uint32_t)cmd->cdb[3] << 16 |
                   (uint32_t)cmd->cdb[4] << 8 | cmd->cdb[5];
    uint32_t nsec = (uint32_t)cmd->cdb[6] << 16 | (uint32_t)cmd->cdb[7] << 8 |
                    cmd->cdb[8];
    unsigned c2 = (cmd->cdb[9] >> 1) & 3u, sub = cmd->cdb[10] & 7u;
    uint32_t rec_len = AUDIO + (c2 == 1 ? C2 : c2 == 2 ? 296u : 0) +
                       (sub == 1 ? SUB : sub == 2 ? 16u : 0);
    uint32_t data = nsec * rec_len;

    fk.reads++;
    fk.last_xfer = cmd->buf_len;
    fk.last_buf = cmd->buf;
    if (fk.reject_rounded && cmd->buf_len > data)
        return ACCUDISC_ERR_IO;

    int pio = (cmd->buf_len & 15u) != 0;
    uint32_t stride = pio ? (rec_len + 15u) & ~15u : rec_len;
    uint8_t *out = cmd->buf;
    uint8_t tmp[REC];

    memset(out, 0, cmd->buf_len); /* DMA: the kernel returns all of dxfer_len */
    for (uint32_t s = 0; s < nsec; s++) {
        uint32_t at = s * stride, cur = lba + s;
        uint8_t rec[4096] = {0};
        uint32_t o = 0;

        for (uint32_t i = 0; i < AUDIO; i++)
            rec[o++] = audio_byte(cur, i);
        if (sub == 1 && fk.sub_first) {
            if (fk.zero_sub) memset(tmp, 0, SUB); else make_sub(cur, 0, tmp);
            memcpy(rec + o, tmp, SUB); o += SUB;
        }
        if (c2) {
            /* clean disc: zero C2, except one fired bit on the chosen LBA */
            if (fk.c2_hot_lba && cur == fk.c2_hot_lba)
                rec[o + 17] = 0x80;
            o += c2 == 1 ? C2 : 296u;
        }
        if (sub == 1 && !fk.sub_first) {
            if (fk.zero_sub) memset(tmp, 0, SUB); else make_sub(cur, 0, tmp);
            memcpy(rec + o, tmp, SUB); o += SUB;
        }
        if (sub == 2)
            o += 16u;
        if (pio && stride > rec_len && s + 1 < nsec) {
            /* the pad: 00 00, then 8 stale bytes of this record's audio */
            for (uint32_t i = 0; i < 8 && o + 2 + i < stride; i++)
                rec[o + 2 + i] = audio_byte(cur, 696 + i);
        }
        for (uint32_t i = 0; i < stride && at + i < cmd->buf_len; i++)
            out[at + i] = rec[i];
    }
    return ACCUDISC_OK;
}

static struct accudisc_device *fresh_dev(void)
{
    static struct accudisc_device dev;

    free(dev.xfer_bounce);
    memset(&dev, 0, sizeof(dev));
    memset(&fk, 0, sizeof(fk));
    return &dev;
}

static void expect_mmc_records(const uint8_t *buf, uint32_t lba, uint32_t nsec)
{
    uint8_t want[REC];

    for (uint32_t s = 0; s < nsec; s++) {
        make_record_mmc(lba + s, want);
        if (memcmp(buf + (size_t)s * REC, want, REC) != 0) {
            fprintf(stderr, "record %u (lba %u) differs\n", s, lba + s);
            assert(0 && "record not in MMC order at the MMC stride");
        }
    }
}

/* ---- pure layout functions ----------------------------------------------- */

static void build_buffer(uint8_t *buf, uint32_t lba, uint32_t nsec, int sub_first,
                         uint32_t stride, uint32_t cap)
{
    uint8_t rec[REC + 16];

    memset(buf, 0, cap);
    for (uint32_t s = 0; s < nsec; s++) {
        memset(rec, 0, sizeof(rec));
        for (uint32_t i = 0; i < AUDIO; i++)
            rec[i] = audio_byte(lba + s, i);
        make_sub(lba + s, 0, rec + AUDIO + (sub_first ? 0 : C2));
        for (uint32_t i = 0; i < stride && (size_t)s * stride + i < cap; i++)
            buf[(size_t)s * stride + i] = rec[i];
    }
}

static void test_inspect(void)
{
    uint32_t n = 23, cap = n * REC;
    uint8_t *a = malloc(cap), *b = malloc(cap);
    adsc_layout_verdict v;

    build_buffer(a, 20010, n, 0, REC, cap);
    adsc_layout_inspect(a, n, REC, C2, &v);
    assert(v.layout == ADSC_LAYOUT_MMC && v.hits_mmc == n && v.hits_sub == 0);
    assert(v.misframed == 0);

    build_buffer(b, 20010, n, 1, REC, cap);
    adsc_layout_inspect(b, n, REC, C2, &v);
    assert(v.layout == ADSC_LAYOUT_SUB_FIRST && v.hits_sub == n && v.hits_mmc == 0);
    assert(v.misframed == 0);

    /* Normalising the sub-first buffer yields the MMC buffer, byte for byte. */
    adsc_layout_to_mmc(b, n, REC, C2);
    assert(memcmp(a, b, cap) == 0);

    /* One record decides for itself. */
    build_buffer(b, 7, 1, 1, REC, REC);
    adsc_layout_inspect(b, 1, REC, C2, &v);
    assert(v.layout == ADSC_LAYOUT_SUB_FIRST);

    /* No CRC-valid Q anywhere: UNKNOWN, never a default, and not misframed. */
    memset(b, 0, cap);
    adsc_layout_inspect(b, n, REC, C2, &v);
    assert(v.layout == ADSC_LAYOUT_UNKNOWN && v.misframed == 0);

    /* Padded to 2752 and truncated at 23 x 2742, in both orders. */
    for (int sf = 0; sf < 2; sf++) {
        build_buffer(b, 40000, n, sf, 2752, cap);
        adsc_layout_inspect(b, n, REC, C2, &v);
        assert(v.misframed == 2752);
        assert(v.layout == ADSC_LAYOUT_UNKNOWN);
    }

    /* Genuinely damaged Q at the right stride — 8 of 23 valid — is NOT
     * misframing: no other stride explains it. */
    build_buffer(a, 60000, n, 0, REC, cap);
    for (uint32_t s = 0; s < n; s++)
        if (s % 3)
            make_sub(60000 + s, 1, a + (size_t)s * REC + AUDIO + C2);
    adsc_layout_inspect(a, n, REC, C2, &v);
    assert(v.hits_mmc == 8 && v.misframed == 0 && v.layout == ADSC_LAYOUT_MMC);

    /* A record that is not raw-P-W shaped is left alone. */
    build_buffer(a, 1, 2, 1, REC, 2 * REC);
    memcpy(b, a, 2 * REC);
    adsc_layout_to_mmc(b, 2, AUDIO + C2 + 16, C2);
    assert(memcmp(a, b, 2 * REC) == 0);

    free(a);
    free(b);
}

/* ---- adsc_mmc_read_cd against the fake drive ----------------------------- */

static void test_read_sub_first_rounded(void)
{
    struct accudisc_device *dev = fresh_dev();
    uint32_t sizes[] = {23, 15, 3, 1, 16, 24};

    fk.sub_first = 1;
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        uint32_t n = sizes[k], lba = 30000 + 1000 * (uint32_t)k;
        uint8_t *buf = malloc((size_t)n * REC);

        int rc = adsc_mmc_read_cd(dev, lba, n, ADSC_SECTOR_CDDA, ADSC_C2_294,
                                  ADSC_SUB_RAW, buf, REC);
        assert(rc == ACCUDISC_OK);
        assert(fk.last_xfer % 16 == 0);
        assert(fk.last_xfer >= n * REC && fk.last_xfer < n * REC + 16);
        /* a rounded transfer never lands in the caller's exact-size buffer */
        if (fk.last_xfer != n * REC)
            assert(fk.last_buf != buf);
        expect_mmc_records(buf, lba, n);
        free(buf);
    }
    assert(dev->layout[ADSC_C2_294] == ADSC_LAYOUT_SUB_FIRST);
}

static void test_read_mmc_drive_untouched(void)
{
    struct accudisc_device *dev = fresh_dev();
    uint8_t *buf = malloc(23u * REC);

    assert(adsc_mmc_read_cd(dev, 100, 23, ADSC_SECTOR_CDDA, ADSC_C2_294,
                            ADSC_SUB_RAW, buf, REC) == ACCUDISC_OK);
    expect_mmc_records(buf, 100, 23);
    assert(dev->layout[ADSC_C2_294] == ADSC_LAYOUT_MMC);
    free(buf);
}

/* THE REGRESSION, reproduced: with rounding off the fake goes PIO, pads, and
 * the read must be REFUSED — the pre-0.39.0 library returned OK here and a
 * whole rip verified 0/11. */
static void test_pio_misframe_refused(void)
{
    struct accudisc_device *dev = fresh_dev();
    uint8_t *buf = malloc(23u * REC);

    fk.sub_first = 1;
    dev->xfer_exact = 1;
    int rc = adsc_mmc_read_cd(dev, 20010, 23, ADSC_SECTOR_CDDA, ADSC_C2_294,
                              ADSC_SUB_RAW, buf, REC);
    assert(fk.last_xfer == 23u * REC); /* the fake really went PIO */
    assert(rc == ACCUDISC_ERR_IO);
    assert(strstr(dev->last_io, "2752") != NULL);

    /* ... and a one-sector read, which cannot be displaced, is still correct. */
    uint8_t one[REC];
    assert(adsc_mmc_read_cd(dev, 20011, 1, ADSC_SECTOR_CDDA, ADSC_C2_294,
                            ADSC_SUB_RAW, one, REC) == ACCUDISC_OK);
    expect_mmc_records(one, 20011, 1);
    free(buf);
}

/* A transport that rejects the rounded transfer falls back to exact lengths,
 * once, and latches — and after a rounded read has succeeded it never does. */
static void test_rounding_fallback(void)
{
    struct accudisc_device *dev = fresh_dev();
    uint8_t *buf = malloc(8u * REC);

    fk.reject_rounded = 1;
    assert(adsc_mmc_read_cd(dev, 500, 8, ADSC_SECTOR_CDDA, ADSC_C2_294,
                            ADSC_SUB_RAW, buf, REC) == ACCUDISC_OK); /* 8x: already mod 16 */
    assert(dev->xfer_exact == 0 && fk.reads == 1);

    uint8_t one[REC];
    assert(adsc_mmc_read_cd(dev, 500, 1, ADSC_SECTOR_CDDA, ADSC_C2_294,
                            ADSC_SUB_RAW, one, REC) == ACCUDISC_OK);
    assert(dev->xfer_exact == 1 && fk.reads == 3); /* rounded failed, exact won */
    assert(adsc_mmc_read_cd(dev, 501, 1, ADSC_SECTOR_CDDA, ADSC_C2_294,
                            ADSC_SUB_RAW, one, REC) == ACCUDISC_OK);
    assert(fk.reads == 4 && fk.last_xfer == REC); /* no rounding attempt now */

    dev = fresh_dev();
    assert(adsc_mmc_read_cd(dev, 500, 1, ADSC_SECTOR_CDDA, ADSC_C2_294,
                            ADSC_SUB_RAW, one, REC) == ACCUDISC_OK);
    assert(dev->xfer_exact == -1);
    fk.reject_rounded = 1;
    assert(adsc_mmc_read_cd(dev, 500, 1, ADSC_SECTOR_CDDA, ADSC_C2_294,
                            ADSC_SUB_RAW, one, REC) == ACCUDISC_ERR_IO);
    assert(fk.reads == 2); /* proven handle: no second attempt doubling a timeout */
    free(buf);
}

/* No Q evidence: the buffer is left exactly as delivered, and the latch from
 * earlier evidence decides. */
static void test_no_evidence_uses_latch(void)
{
    struct accudisc_device *dev = fresh_dev();
    uint8_t *buf = malloc(4u * REC);

    fk.sub_first = 1;
    fk.zero_sub = 1;
    assert(adsc_mmc_read_cd(dev, 9, 4, ADSC_SECTOR_CDDA, ADSC_C2_294,
                            ADSC_SUB_RAW, buf, REC) == ACCUDISC_OK);
    assert(dev->layout[ADSC_C2_294] == ADSC_LAYOUT_UNKNOWN);
    for (uint32_t s = 0; s < 4; s++)
        for (uint32_t i = 0; i < AUDIO; i++)
            assert(buf[(size_t)s * REC + i] == audio_byte(9 + s, i));
    free(buf);
}

/* Audio and C2-only reads are rounded too, and are otherwise untouched. */
static void test_other_combos(void)
{
    struct accudisc_device *dev = fresh_dev();
    uint32_t c2only = AUDIO + C2;
    uint8_t *buf = malloc(20u * c2only);

    fk.sub_first = 1;
    assert(adsc_mmc_read_cd(dev, 53000, 20, ADSC_SECTOR_CDDA, ADSC_C2_294,
                            ADSC_SUB_NONE, buf, c2only) == ACCUDISC_OK);
    assert(fk.last_xfer == 52928); /* 20 x 2646 = 52920, rounded */
    for (uint32_t s = 0; s < 20; s++)
        for (uint32_t i = 0; i < AUDIO; i += 97)
            assert(buf[(size_t)s * c2only + i] == audio_byte(53000 + s, i));
    free(buf);

    uint8_t a[27u * AUDIO];
    assert(adsc_mmc_read_cd(dev, 80000, 27, ADSC_SECTOR_CDDA, ADSC_C2_NONE,
                            ADSC_SUB_NONE, a, AUDIO) == ACCUDISC_OK);
    assert(fk.last_xfer == 27u * AUDIO && fk.last_buf == a);
}

/* ---- the whole engine, as cdda2img's rip called it ----------------------- */

static struct {
    uint32_t sectors;
    uint32_t bad;
    int shape_ok;
} ck;

static int check_sink(void *user, const accudisc_chunk *c)
{
    uint8_t want[REC];

    (void)user;
    ck.shape_ok &= c->sector_len == REC && c->audio_len == AUDIO &&
                   c->c2_len == C2 && c->sub_len == SUB;
    for (uint32_t s = 0; s < c->nsec; s++) {
        make_record_mmc(c->lba + s, want);
        ck.bad += memcmp(c->data + (size_t)s * c->sector_len, want, REC) != 0;
    }
    ck.sectors += c->nsec;
    return 0;
}

/* accudisc_read_cdda with C2 + raw P-W against the sub-first fake, default
 * chunking (23 x 2742, the size that went PIO on the rip). Every delivered
 * record must be in MMC order at the MMC stride, and the engine's own
 * statistics must say what a clean disc says: no C2 flags (before 0.39.0 the
 * C2 popcount read the subchannel, so all 92 were flagged) and every Q frame
 * valid (before, 0). Then the same with rounding forced off, which makes the
 * fake pad and the chunk read refused: the engine's single-sector fallback
 * must still deliver correct records. */
static void test_engine_end_to_end(void)
{
    for (int forced_pio = 0; forced_pio < 2; forced_pio++) {
        struct accudisc_device *dev = fresh_dev();
        accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
        accudisc_read_stats st = ACCUDISC_READ_STATS_INIT;

        fk.sub_first = 1;
        dev->xfer_exact = forced_pio;
        memset(&ck, 0, sizeof(ck));
        ck.shape_ok = 1;
        req.lba = 20000;
        req.count = 92;
        req.c2 = ACCUDISC_C2_PTRS;
        req.sub = ACCUDISC_SUB_RAW;
        req.buffer_bytes = ACCUDISC_BUFFER_NONE;

        assert(accudisc_read_cdda(dev, &req, check_sink, NULL, &st) ==
               ACCUDISC_OK);
        assert(ck.shape_ok && ck.sectors == 92);
        if (ck.bad)
            fprintf(stderr, "forced_pio=%d: %u bad records\n", forced_pio, ck.bad);
        assert(ck.bad == 0);
        assert(st.hard_errors == 0 && st.sectors_read == 92);
        assert(st.sectors_flagged == 0);
        assert(st.subq_total == 92 && st.subq_ok == 92);
        assert(st.subq_misposition == 0);
    }

    /* Zero flagged is an ABSENCE, and an absence cannot tell "the popcount reads
     * the C2 field" from "it reads something that happens to be zero". Fire one
     * bit in one sector's C2 and require exactly that sector and that bit. */
    struct accudisc_device *dev = fresh_dev();
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st = ACCUDISC_READ_STATS_INIT;

    fk.sub_first = 1;
    fk.c2_hot_lba = 20041;
    memset(&ck, 0, sizeof(ck));
    ck.shape_ok = 1;
    req.lba = 20000;
    req.count = 92;
    req.c2 = ACCUDISC_C2_PTRS;
    req.sub = ACCUDISC_SUB_RAW;
    req.buffer_bytes = ACCUDISC_BUFFER_NONE;
    assert(accudisc_read_cdda(dev, &req, NULL, NULL, &st) == ACCUDISC_OK);
    assert(st.sectors_flagged == 1 && st.c2_bits == 1);
    assert(st.first_flagged_lba == 20041 && st.last_flagged_lba == 20041);
    assert(st.subq_ok == 92);
}

int main(void)
{
    test_engine_end_to_end();
    test_inspect();
    test_read_sub_first_rounded();
    test_read_mmc_drive_untouched();
    test_pio_misframe_refused();
    test_rounding_fallback();
    test_no_evidence_uses_latch();
    test_other_combos();
    return 0;
}
