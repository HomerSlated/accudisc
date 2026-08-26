/* Status-map byte encoding tests. */

#include <assert.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "cdda/crc16.h"
#include "read/engine.h"

/* Seal a 12-byte Q frame with the CRC accudisc_q_parse will accept: it compares
 * adsc_crc16(q, 10) against the COMPLEMENT of the stored trailer, so store the
 * complement. Building real frames matters — a test that hand-set `crc_ok` on
 * an accudisc_q would never exercise the parse step where adr survives a
 * failed CRC, which is the behaviour the classifier has to defend against. */
static void q_seal(uint8_t q[12])
{
    uint16_t c = (uint16_t)~adsc_crc16(q, 10);

    q[10] = (uint8_t)(c >> 8);
    q[11] = (uint8_t)(c & 0xff);
}

/* Write an absolute MSF into a position frame, BCD, from an LBA. */
static void q_set_abs(uint8_t q[12], int32_t lba)
{
    uint8_t m, sec, f;

    accudisc_lba_to_msf(lba, &m, &sec, &f);
    q[7] = (uint8_t)(((m / 10) << 4) | (m % 10));
    q[8] = (uint8_t)(((sec / 10) << 4) | (sec % 10));
    q[9] = (uint8_t)(((f / 10) << 4) | (f % 10));
}

static uint8_t classify(const uint8_t q[12], accudisc_q *out,
                        uint32_t expect_lba)
{
    accudisc_q parsed;

    accudisc_q_parse(q, out ? out : &parsed);
    return adsc_subq_byte(out ? out : &parsed, expect_lba);
}

static void test_subq_bytes(void)
{
    uint8_t q[12];
    accudisc_q parsed;

    /* A healthy position frame: CTRL=0 in the high nibble, ADR=1 in the low. */
    memset(q, 0, sizeof q);
    q[0] = 0x01;
    q[1] = 0x01;  /* track 1, BCD */
    q[2] = 0x01;  /* index 1 */
    q_seal(q);
    q_set_abs(q, 224850);
    q_seal(q);
    assert(classify(q, &parsed, 224850) == ACCUDISC_SUBQ_OK);
    assert(parsed.crc_ok && parsed.adr == ACCUDISC_Q_POSITION);

    /* THE MISPOSITION TRAP, and the reason this state exists. The SAME frame —
     * valid CRC, ADR=1, internally consistent — is healthy when it names the
     * sector we asked for and a fault when it does not. A drive that lost lock
     * and re-acquired elsewhere emits exactly this: nothing about the frame is
     * malformed, so every check that inspects only the frame calls it OK.
     * Measured on a PX-716A at 2048 sectors of displacement. */
    assert(classify(q, NULL, 224851) == ACCUDISC_SUBQ_MISPOSITION);
    assert(classify(q, NULL, 224850 - 2048) == ACCUDISC_SUBQ_MISPOSITION);
    assert(classify(q, NULL, 224850) == ACCUDISC_SUBQ_OK);

    /* Off by ONE must fail. A margin belongs in the engine, which widens a
     * detected run; the classifier itself has to be exact, or a one-sector
     * slip — the smallest real one — reads as health. */
    assert(classify(q, NULL, 224849) == ACCUDISC_SUBQ_MISPOSITION);

    /* And the state must not collide with the others in the lane. */
    assert(ACCUDISC_SUBQ_MISPOSITION != ACCUDISC_SUBQ_OK &&
           ACCUDISC_SUBQ_MISPOSITION != ACCUDISC_SUBQ_BAD &&
           ACCUDISC_SUBQ_MISPOSITION != ACCUDISC_SUBQ_NO_POSITION &&
           ACCUDISC_SUBQ_MISPOSITION != ACCUDISC_SUBQ_NO_AUDIO &&
           ACCUDISC_SUBQ_MISPOSITION != ACCUDISC_SUBQ_PENDING);
    assert(ACCUDISC_SUBQ_STATE(ACCUDISC_SUBQ_MISPOSITION) ==
           ACCUDISC_SUBQ_MISPOSITION);

    /* A MISPOSITION frame must still decode its position for the caller. */
    {
        accudisc_q qd;
        classify(q, &qd, 224851);
        assert(adsc_q_position_lba(&qd) == 224850);
    }

    /* MCN and ISRC frames are healthy too — they are interleaved into the
     * position stream by the pressing, not a symptom of anything. */
    memset(q, 0, sizeof q);
    q[0] = 0x02;
    q_seal(q);
    assert(classify(q, NULL, (uint32_t)-150) == ACCUDISC_SUBQ_NO_POSITION);

    memset(q, 0, sizeof q);
    q[0] = 0x03;
    q_seal(q);
    assert(classify(q, NULL, (uint32_t)-150) == ACCUDISC_SUBQ_NO_POSITION);

    /* THE POLARITY TRAP. A frame whose CRC fails but whose header byte still
     * decodes to ADR=2 must be BAD, never NO_POSITION — the latter is the
     * lane's HEALTHY state, so getting this backwards reports damage as health
     * on exactly the frames the lane exists to find.
     *
     * The two asserts before the verdict are the point of the case: they prove
     * the input can actually distinguish the two orderings. Without them a
     * frame that merely failed to parse as ADR=2 would satisfy the verdict
     * while testing nothing. */
    memset(q, 0, sizeof q);
    q[0] = 0x02;
    q_seal(q);
    q[3] ^= 0xff; /* payload damage: CRC now fails, header byte untouched */
    classify(q, &parsed, (uint32_t)-150);
    assert(!parsed.crc_ok);                 /* the CRC really did fail... */
    assert(parsed.adr == ACCUDISC_Q_MCN);   /* ...and adr really is still 2 */
    assert(classify(q, NULL, (uint32_t)-150) == ACCUDISC_SUBQ_BAD);

    /* Same trap from the other direction: damage that lands IN the header byte
     * and turns a position frame into a plausible-looking MCN one. */
    memset(q, 0, sizeof q);
    q[0] = 0x01;
    q[1] = 0x01;
    q_seal(q);
    q[0] = 0x02;
    classify(q, &parsed, (uint32_t)-150);
    assert(!parsed.crc_ok && parsed.adr == ACCUDISC_Q_MCN);
    assert(classify(q, NULL, (uint32_t)-150) == ACCUDISC_SUBQ_BAD);

    /* The measurement the whole lane rests on: the frame a hard-unreadable
     * sector delivers is zero-filled, and it FAILS CRC-16 rather than being
     * recognised as absent. So a consumer recomputing this lane downstream
     * records fabricated Q damage on every sector whose audio is already gone.
     * The engine stores NO_AUDIO there instead, before parsing anything. */
    memset(q, 0x00, sizeof q);
    assert(classify(q, NULL, (uint32_t)-150) == ACCUDISC_SUBQ_BAD);
    memset(q, 0xff, sizeof q);
    assert(classify(q, NULL, (uint32_t)-150) == ACCUDISC_SUBQ_BAD);

    /* Severity is always zero: one CRC over one frame has no gradient. */
    memset(q, 0, sizeof q);
    q[0] = 0x01;
    q_seal(q);
    assert((classify(q, NULL, (uint32_t)-150) >> 4) == 0);
    q[3] ^= 0xff;
    assert((classify(q, NULL, (uint32_t)-150) >> 4) == 0);

    assert(ACCUDISC_SUBQ_STATE(ACCUDISC_SUBQ_NO_AUDIO) ==
           ACCUDISC_SUBQ_NO_AUDIO);

    /* The numbering collides with ACCUDISC_MAP_* by design (parallel shape, one
     * renderer), and the vocabularies are disjoint. Pinned so that the day
     * someone decodes a subq byte with ACCUDISC_MAP_STATE they can be pointed
     * at a deliberate decision rather than an accident: NO_AUDIO would read
     * back as RECOVERED, and BAD as C2, both perfectly well-formed. */
    assert(ACCUDISC_SUBQ_NO_AUDIO == ACCUDISC_MAP_RECOVERED);
    assert(ACCUDISC_SUBQ_BAD == ACCUDISC_MAP_C2);
}

int main(void)
{
    test_subq_bytes();

    /* States and severities round-trip through the accessor macros. */
    assert(ACCUDISC_MAP_STATE(ACCUDISC_MAP_OK) == ACCUDISC_MAP_OK);
    assert(ACCUDISC_MAP_SEVERITY(ACCUDISC_MAP_OK) == 0);

    /* C2 severity is ~log2(bits)+1, clamped to the nibble. */
    uint8_t b1 = adsc_map_c2_byte(1);
    assert(ACCUDISC_MAP_STATE(b1) == ACCUDISC_MAP_C2);
    assert(ACCUDISC_MAP_SEVERITY(b1) == 1);

    assert(ACCUDISC_MAP_SEVERITY(adsc_map_c2_byte(2)) == 2);
    assert(ACCUDISC_MAP_SEVERITY(adsc_map_c2_byte(3)) == 2);
    assert(ACCUDISC_MAP_SEVERITY(adsc_map_c2_byte(4)) == 3);
    assert(ACCUDISC_MAP_SEVERITY(adsc_map_c2_byte(255)) == 8);
    /* Worst case: all 2352 bits fired. */
    assert(ACCUDISC_MAP_SEVERITY(adsc_map_c2_byte(2352)) == 12);
    /* Clamp holds even for impossible counts. */
    assert(ACCUDISC_MAP_SEVERITY(adsc_map_c2_byte(0xffffffffu)) == 15);

    /* Severity never collides state into another nibble. */
    assert(ACCUDISC_MAP_STATE(adsc_map_c2_byte(0xffffffffu)) ==
           ACCUDISC_MAP_C2);

    /* Recovered: severity = attempts, clamped. */
    uint8_t r = adsc_map_recovered_byte(3);
    assert(ACCUDISC_MAP_STATE(r) == ACCUDISC_MAP_RECOVERED);
    assert(ACCUDISC_MAP_SEVERITY(r) == 3);
    assert(ACCUDISC_MAP_SEVERITY(adsc_map_recovered_byte(99)) == 15);

    /* Suspect: severity ~log2 of disagreeing bytes. */
    uint8_t s = adsc_map_suspect_byte(256);
    assert(ACCUDISC_MAP_STATE(s) == ACCUDISC_MAP_SUSPECT);
    assert(ACCUDISC_MAP_SEVERITY(s) == 9);
    assert(ACCUDISC_MAP_SEVERITY(adsc_map_suspect_byte(1)) == 1);

    /* Audio diff counts differing bytes over one sector. */
    uint8_t a[2352] = {0}, b[2352] = {0};
    assert(adsc_audio_diff(a, b) == 0);
    b[0] = 1;
    b[2351] = 0xff;
    assert(adsc_audio_diff(a, b) == 2);

    /* Slip detection: b = a shifted by +12 samples must be found; the
     * overlap must verify end to end. */
    uint8_t x[2352], y[2352];
    for (int i = 0; i < 2352; i++)
        x[i] = (uint8_t)(i * 7 + (i >> 3)); /* signal-bearing pattern */
    memset(y, 0xAA, sizeof(y));
    memcpy(y + 48, x, 2352 - 48); /* y holds x delayed by 12 samples */
    int32_t sh = 0;
    assert(adsc_shift_find(x, y, &sh) == 1);
    assert(sh == 12);

    /* Negative shift too. */
    memset(y, 0x55, sizeof(y));
    memcpy(y, x + 20, 2352 - 20); /* y holds x advanced by 5 samples */
    assert(adsc_shift_find(x, y, &sh) == 1);
    assert(sh == -5);

    /* Genuine in-place damage is NOT a slip. */
    memcpy(y, x, 2352);
    y[1200] ^= 0xff;
    assert(adsc_shift_find(x, y, &sh) == 0);

    /* Silence carries no positional signal: no verdict, no false slip. */
    memset(x, 0, sizeof(x));
    memset(y, 0, sizeof(y));
    y[0] = 1;
    assert(adsc_shift_find(x, y, &sh) == 0);

    return 0;
}
