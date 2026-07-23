/* Stage-3 decoder tests against real captured vectors (see vectors.h).
 * Ground truth: the cdda2img disc_scan.py reference output for the ABBA
 * "Gold: Greatest Hits" disc (MCN, ISRCs, CD-Text). */

#include <assert.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "vectors.h"

static void test_msf(void)
{
    assert(accudisc_msf_to_lba(0, 2, 0) == 0);
    assert(accudisc_msf_to_lba(0, 2, 1) == 1);
    assert(accudisc_msf_to_lba(77, 9, 0) == 347025);

    uint8_t m, s, f;
    accudisc_lba_to_msf(347175, &m, &s, &f);
    assert(accudisc_msf_to_lba(m, s, f) == 347175);
    accudisc_lba_to_msf(0, &m, &s, &f);
    assert(m == 0 && s == 2 && f == 0);

    /* F-007: an LBA below the lead-in (< -150) is before 00:00:00 — clamp to
     * 00:00:00 rather than casting a negative quotient to uint8_t garbage. */
    accudisc_lba_to_msf(-150, &m, &s, &f); /* exactly 00:00:00 (not clamped) */
    assert(m == 0 && s == 0 && f == 0);
    accudisc_lba_to_msf(-75, &m, &s, &f);  /* 00:01:00 (representable, unclamped) */
    assert(m == 0 && s == 1 && f == 0);
    accudisc_lba_to_msf(-1000, &m, &s, &f); /* below 00:00:00 -> clamped */
    assert(m == 0 && s == 0 && f == 0);
}

static void test_q_position(void)
{
    uint8_t q[12];
    accudisc_q parsed;

    accudisc_sub_extract_q(vec_sub_adr1, q);
    assert(accudisc_q_parse(q, &parsed) == ACCUDISC_OK);
    assert(parsed.crc_ok);
    assert(parsed.adr == ACCUDISC_Q_POSITION);
    /* Captured at sector 1: track 1 index 1, rel 00:00:01, abs 00:02:01. */
    assert(parsed.tno == 1 && parsed.index == 1);
    assert(parsed.rel_m == 0 && parsed.rel_s == 0 && parsed.rel_f == 1);
    assert(parsed.abs_m == 0 && parsed.abs_s == 2 && parsed.abs_f == 1);
    assert(accudisc_msf_to_lba(parsed.abs_m, parsed.abs_s, parsed.abs_f)
           == 1);
    /* Audio track: data bit (control bit 2) clear. */
    assert((parsed.control & 0x4) == 0);
}

static void test_q_mcn(void)
{
    uint8_t q[12];
    accudisc_q parsed;

    accudisc_sub_extract_q(vec_sub_adr2, q);
    assert(accudisc_q_parse(q, &parsed) == ACCUDISC_OK);
    assert(parsed.adr == ACCUDISC_Q_MCN);
    assert(strcmp(parsed.mcn, "0731451700729") == 0);
}

static void test_q_isrc(void)
{
    uint8_t q[12];
    accudisc_q parsed;

    accudisc_sub_extract_q(vec_sub_adr3, q);
    assert(accudisc_q_parse(q, &parsed) == ACCUDISC_OK);
    assert(parsed.adr == ACCUDISC_Q_ISRC);
    assert(strcmp(parsed.isrc, "SEAYD7601020") == 0); /* track 1 */
}

static void test_q_crc_rejects_corruption(void)
{
    uint8_t q[12];
    accudisc_q parsed;

    accudisc_sub_extract_q(vec_sub_adr2, q);
    q[3] ^= 0x10; /* flip one MCN digit bit */
    assert(accudisc_q_parse(q, &parsed) == ACCUDISC_ERR_CRC);
    assert(!parsed.crc_ok);
    /* F-006: a CRC-failed frame must not leave decoded payload behind — the
     * MCN string stays empty rather than carrying plausible-looking garbage. */
    assert(parsed.mcn[0] == '\0');
}

static void test_fulltoc(void)
{
    accudisc_fulltoc ft;

    assert(accudisc_fulltoc_parse(vec_fulltoc, sizeof(vec_fulltoc), &ft)
           == ACCUDISC_OK);
    assert(ft.first_session == 1 && ft.last_session == 1);
    assert(ft.entry_count == 22); /* A0 A1 A2 + 19 tracks */

    int tracks = 0;
    for (uint16_t i = 0; i < ft.entry_count; i++) {
        const accudisc_fulltoc_entry *e = &ft.entries[i];

        if (e->point == 0xA0) {
            assert(e->pmin == 1);  /* first track */
            assert(e->psec == 0);  /* disc type: CD-DA */
        } else if (e->point == 0xA1) {
            assert(e->pmin == 19); /* last track */
        } else if (e->point == 0xA2) {
            assert(accudisc_msf_to_lba(e->pmin, e->psec, e->pframe)
                   == 347175);
        } else if (e->point >= 1 && e->point <= 0x63) {
            if (e->point == 1)
                assert(accudisc_msf_to_lba(e->pmin, e->psec, e->pframe)
                       == 0);
            tracks++;
        }
    }
    assert(tracks == 19);

    assert(accudisc_fulltoc_parse(vec_fulltoc, 3, &ft)
           == ACCUDISC_ERR_SHORT);
}

static void test_cdtext(void)
{
    accudisc_cdtext *text = NULL;

    assert(accudisc_cdtext_decode(vec_cdtext, sizeof(vec_cdtext), &text)
           == ACCUDISC_OK);
    assert(strcmp(text->album.title, "Gold: Greatest Hits") == 0);
    assert(strcmp(text->album.performer, "ABBA") == 0);
    assert(strcmp(text->track[1].title, "Dancing Queen") == 0);
    assert(strcmp(text->track[1].performer, "ABBA") == 0);
    assert(strcmp(text->track[14].title,
                  "Gimme! Gimme! Gimme! (A Man After Midnight)") == 0);
    assert(strcmp(text->track[19].title, "Waterloo") == 0);
    /* Non-ASCII pass-through: track 13's U+2010 hyphen survives as the
     * disc's own UTF-8 bytes (cdrdao famously mangles this to mojibake). */
    assert(strcmp(text->track[13].title,
                  "Voulez\xe2\x80\x90Vous (edit)") == 0);
    accudisc_free(text);

    /* A blob with no usable packs is a soft absence. */
    uint8_t junk[4 + 18] = {0};
    assert(accudisc_cdtext_decode(junk, sizeof(junk), &text)
           == ACCUDISC_ERR_SHORT);
}

int main(void)
{
    test_msf();
    test_q_position();
    test_q_mcn();
    test_q_isrc();
    test_q_crc_rejects_corruption();
    test_fulltoc();
    test_cdtext();
    return 0;
}
