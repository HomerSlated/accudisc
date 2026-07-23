/* Q subchannel decode: extraction from the raw interleaved P-W stream and
 * frame parsing per IEC 60908 (ADR 1 position, ADR 2 MCN, ADR 3 ISRC). */

#include <string.h>

#include <accudisc/accudisc.h>

#include "crc16.h"

int32_t accudisc_msf_to_lba(uint8_t m, uint8_t s, uint8_t f)
{
    return ((int32_t)m * 60 + s) * 75 + f - 150;
}

void accudisc_lba_to_msf(int32_t lba, uint8_t *m, uint8_t *s, uint8_t *f)
{
    lba += 150;
    if (lba < 0)
        lba = 0; /* below 00:00:00 (deep lead-in): MSF can't represent it, and
                  * casting a negative quotient to uint8_t is garbage. Clamp,
                  * matching the write path's put_msf. */
    if (m)
        *m = (uint8_t)(lba / (60 * 75));
    if (s)
        *s = (uint8_t)((lba / 75) % 60);
    if (f)
        *f = (uint8_t)(lba % 75);
}

void accudisc_sub_extract_q(const uint8_t raw[96], uint8_t q[12])
{
    memset(q, 0, 12);
    /* One bit of each subchannel per byte, Q at bit 6; 96 bits, MSB-first. */
    for (unsigned i = 0; i < 96; i++)
        q[i >> 3] = (uint8_t)((q[i >> 3] << 1) | ((raw[i] >> 6) & 1));
}

static uint8_t bcd(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 0x0f));
}

/* 6-bit ISRC character: 0-9 and A-Z share the "+ 0x30" mapping. */
static char isrc_char(unsigned v)
{
    return (char)(v + 0x30);
}

int accudisc_q_parse(const uint8_t q[12], accudisc_q *out)
{
    memset(out, 0, sizeof(*out));
    out->control = (uint8_t)(q[0] >> 4);
    out->adr = (uint8_t)(q[0] & 0x0f);

    uint16_t want = (uint16_t)~(((uint16_t)q[10] << 8) | q[11]);
    out->crc_ok = adsc_crc16(q, 10) == want;

    /* Decode payload only for a CRC-good frame. On a bad frame the BCD/ISRC
     * decoders would emit values > 99 (unvalidated nibbles) or non-alphanumeric
     * ISRC chars; leaving the fields zeroed (memset above) keeps the struct from
     * handing back plausible-looking garbage to a caller that ignores the
     * return. adr/control (the frame-type header) stay set either way. */
    if (!out->crc_ok)
        return ACCUDISC_ERR_CRC;

    switch (out->adr) {
    case ACCUDISC_Q_POSITION:
        out->tno = bcd(q[1]);
        out->index = bcd(q[2]);
        out->rel_m = bcd(q[3]);
        out->rel_s = bcd(q[4]);
        out->rel_f = bcd(q[5]);
        out->abs_m = bcd(q[7]);
        out->abs_s = bcd(q[8]);
        out->abs_f = bcd(q[9]);
        break;
    case ACCUDISC_Q_MCN:
        /* 13 BCD digits packed in q[1..7] (14th nibble zero). */
        for (unsigned i = 0; i < 13; i++) {
            uint8_t byte = q[1 + i / 2];
            uint8_t nib = (i & 1) ? (byte & 0x0f) : (byte >> 4);
            out->mcn[i] = (char)('0' + nib);
        }
        out->mcn[13] = '\0';
        break;
    case ACCUDISC_Q_ISRC:
        /* Five 6-bit chars (country + owner) then seven BCD digits
         * (year + designation); q[4] bits 1-0 are zero-padding. */
        out->isrc[0] = isrc_char(q[1] >> 2);
        out->isrc[1] = isrc_char(((q[1] & 0x03) << 4) | (q[2] >> 4));
        out->isrc[2] = isrc_char(((q[2] & 0x0f) << 2) | (q[3] >> 6));
        out->isrc[3] = isrc_char(q[3] & 0x3f);
        out->isrc[4] = isrc_char(q[4] >> 2);
        for (unsigned i = 0; i < 7; i++) {
            uint8_t byte = q[5 + i / 2];
            uint8_t nib = (i & 1) ? (byte & 0x0f) : (byte >> 4);
            out->isrc[5 + i] = (char)('0' + nib);
        }
        out->isrc[12] = '\0';
        break;
    default:
        break;
    }
    return ACCUDISC_OK; /* the !crc_ok path returned ERR_CRC above */
}
