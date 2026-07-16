/* accudisc_index_map_decode: classify per-track pregap state from a synthesized
 * raw-subchannel scan. Covers PRESENT (clean), NONE (gapless), UNKNOWN (damaged
 * approach), PRESENT-with-a-dead-start-frame (length becomes a lower bound), and
 * NO_DATA (boundary outside the scan). */

#include <assert.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "cdda/crc16.h"

static uint8_t bcd(uint8_t v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

/* Pack one ADR=1 position frame into a raw 96-byte subcode block (Q at bit 6).
 * If corrupt, damage the frame after the CRC is written so it fails to verify. */
static void put_pos(uint8_t *raw96, uint8_t control, uint8_t tno, uint8_t index,
                    uint32_t rel_fr, int32_t abs_lba, int corrupt)
{
    uint8_t q[12] = {0};
    q[0] = (uint8_t)(control << 4 | ACCUDISC_Q_POSITION);
    q[1] = bcd(tno);
    q[2] = bcd(index);
    q[3] = bcd((uint8_t)(rel_fr / (75 * 60)));
    q[4] = bcd((uint8_t)((rel_fr / 75) % 60));
    q[5] = bcd((uint8_t)(rel_fr % 75));
    uint8_t am, as, af;
    accudisc_lba_to_msf(abs_lba, &am, &as, &af);
    q[7] = bcd(am);
    q[8] = bcd(as);
    q[9] = bcd(af);
    uint16_t want = (uint16_t)~adsc_crc16(q, 10);
    q[10] = (uint8_t)(want >> 8);
    q[11] = (uint8_t)(want & 0xff);
    if (corrupt)
        q[3] ^= 0x10; /* break the CRC */

    memset(raw96, 0, 96);
    for (unsigned i = 0; i < 96; i++)
        raw96[i] = (uint8_t)(((q[i >> 3] >> (7 - (i & 7))) & 1) << 6);
}

/* Layout on a 0..799 scan (base_lba 0):
 *   [0,49]   lead-in     (tno 0, idx 0)
 *   [50,99]  t1 pregap   (tno 1, idx 0)  -> L1 = 100, 50-frame pregap, clean
 *   [100,299] t1 body    (tno 1, idx 1)
 *   [300,499] t2 body    (tno 2, idx 1)  -> L2 = 300 gapless; 499 corrupted
 *   [500,659] t3 body    (tno 3, idx 1)  -> L3 = 500 damaged approach
 *   [660,699] t4 pregap  (tno 4, idx 0)  -> L4 = 700, 40-frame pregap, 660 dead
 *   [700,799] t4 body    (tno 4, idx 1)
 *   t5 at L5 = 900 lies outside the scan.
 */
#define N 800
static void build_scan(uint8_t *raw)
{
    for (int32_t lba = 0; lba < N; lba++) {
        uint8_t *b = raw + (size_t)lba * 96;
        if (lba <= 49)
            put_pos(b, 0, 0, 0, (uint32_t)(100 - lba), lba, 0); /* lead-in */
        else if (lba <= 99)
            put_pos(b, 0, 1, 0, (uint32_t)(100 - lba), lba, 0); /* t1 pregap */
        else if (lba <= 299)
            put_pos(b, 0, 1, 1, (uint32_t)(lba - 100), lba, 0); /* t1 body */
        else if (lba <= 499)
            put_pos(b, 0, 2, 1, (uint32_t)(lba - 300), lba, lba == 499);
        else if (lba <= 659)
            put_pos(b, 0, 3, 1, (uint32_t)(lba - 500), lba, 0); /* t3 body */
        else if (lba <= 699)
            put_pos(b, 0, 4, 0, (uint32_t)(700 - lba), lba, lba == 660);
        else
            put_pos(b, 0, 4, 1, (uint32_t)(lba - 700), lba, 0); /* t4 body */
    }
}

static accudisc_toc mk_toc(void)
{
    accudisc_toc toc;
    memset(&toc, 0, sizeof(toc));
    toc.first_track = 1;
    toc.last_track = 5;
    toc.track_count = 5;
    const uint32_t starts[5] = {100, 300, 500, 700, 900};
    for (int i = 0; i < 5; i++) {
        toc.tracks[i].number = (uint8_t)(i + 1);
        toc.tracks[i].lba = starts[i];
    }
    return toc;
}

int main(void)
{
    static uint8_t raw[N * 96];
    build_scan(raw);
    accudisc_toc toc = mk_toc();
    accudisc_index_map map[5];

    uint32_t n = accudisc_index_map_decode(raw, 0, N, &toc, map, 5);
    assert(n == 5);

    /* t1: clean 50-frame pregap starting at LBA 50. */
    assert(map[0].track == 1);
    assert(map[0].pregap_state == ACCUDISC_PREGAP_PRESENT);
    assert(map[0].pregap_frames == 50);
    assert(map[0].index0_lba == 50);
    assert(map[0].index1_lba == 100);
    assert(map[0].q_index1_lba == 100);
    assert(map[0].crc_bad == 0);

    /* t2: gapless — clean approach, no index-0. */
    assert(map[1].pregap_state == ACCUDISC_PREGAP_NONE);
    assert(map[1].pregap_frames == 0);
    assert(map[1].index0_lba == -1);
    assert(map[1].crc_bad == 0);

    /* t3: damaged approach (499 CRC-bad), no index-0 -> indeterminate. */
    assert(map[2].pregap_state == ACCUDISC_PREGAP_UNKNOWN);
    assert(map[2].crc_bad == 1);
    assert(map[2].index0_lba == -1);
    assert(map[2].q_index1_lba == 500);

    /* t4: pregap present but its start frame (rel 40) is dead, so the length
     * is the surviving lower bound of 39, not 40. Still PRESENT. */
    assert(map[3].pregap_state == ACCUDISC_PREGAP_PRESENT);
    assert(map[3].pregap_frames == 39);
    assert(map[3].index0_lba == 661);
    assert(map[3].crc_bad == 1);

    /* t5: boundary outside the scan. */
    assert(map[4].pregap_state == ACCUDISC_PREGAP_NO_DATA);
    assert(map[4].index1_lba == 900);
    assert(map[4].index0_lba == -1);

    return 0;
}
