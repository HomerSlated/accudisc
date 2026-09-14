#include "layout.h"

#include <string.h>

#include <accudisc/accudisc.h>

#define SUB_RAW 96u
#define AUDIO 2352u

/* Probe no further than this many extra bytes per record. The one padding
 * measured is to a 16-byte boundary (+10 on 2742); a stride further out than
 * that is not a padding artefact and is not ours to guess at. */
#define MISFRAME_SPAN 16u
#define MISFRAME_MIN_SECTORS 4u

/* The library's own Q extraction and CRC, not a second copy of them. */
static int q_valid(const uint8_t *raw)
{
    uint8_t q[12];
    accudisc_q qd;

    accudisc_sub_extract_q(raw, q);
    accudisc_q_parse(q, &qd);
    return qd.crc_ok;
}

uint32_t adsc_layout_q_hits(const uint8_t *buf, uint32_t buf_len,
                            uint32_t nsec, uint32_t stride, uint32_t sub_off)
{
    uint32_t hits = 0;

    for (uint32_t s = 0; s < nsec; s++) {
        uint64_t at = (uint64_t)s * stride + sub_off;

        if (at + SUB_RAW > buf_len)
            break;
        hits += (uint32_t)q_valid(buf + at);
    }
    return hits;
}

/* One position wins when it has hits and the other has at most a third as
 * many. A CRC-16 collision in C2 bytes or audio is ~1 in 65 536 per record, so
 * a genuine split vote means the buffer is not what either model says, and it
 * is reported as UNKNOWN rather than resolved by a coin. */
static int winner(uint32_t mmc, uint32_t sub)
{
    if (mmc > 0 && sub * 3 <= mmc)
        return ADSC_LAYOUT_MMC;
    if (sub > 0 && mmc * 3 <= sub)
        return ADSC_LAYOUT_SUB_FIRST;
    return ADSC_LAYOUT_UNKNOWN;
}

void adsc_layout_inspect(const uint8_t *buf, uint32_t nsec,
                         uint32_t sector_len, uint32_t c2_len,
                         adsc_layout_verdict *v)
{
    uint32_t len = nsec * sector_len;

    memset(v, 0, sizeof(*v));
    v->hits_mmc = adsc_layout_q_hits(buf, len, nsec, sector_len, AUDIO + c2_len);
    v->hits_sub = adsc_layout_q_hits(buf, len, nsec, sector_len, AUDIO);
    v->layout = winner(v->hits_mmc, v->hits_sub);

    uint32_t best = v->hits_mmc > v->hits_sub ? v->hits_mmc : v->hits_sub;

    if (nsec < MISFRAME_MIN_SECTORS || best * 2 >= nsec)
        return;

    /* Record 0 always sits at offset 0 whatever the stride, so it votes for
     * every candidate equally; count records 1.. only, or a 4-record buffer
     * would need just one displaced hit to look explained. */
    uint32_t tail = nsec - 1;

    for (uint32_t stride = sector_len + 1; stride <= sector_len + MISFRAME_SPAN;
         stride++) {
        for (unsigned order = 0; order < 2; order++) {
            uint32_t off = order ? AUDIO : AUDIO + c2_len;
            uint32_t h = adsc_layout_q_hits(buf, len, nsec, stride, off) -
                         (uint32_t)q_valid(buf + off);

            if (h * 2 >= tail && h >= 2) {
                v->misframed = stride;
                v->layout = ADSC_LAYOUT_UNKNOWN;
                return;
            }
        }
    }
}

void adsc_layout_to_mmc(uint8_t *buf, uint32_t nsec, uint32_t sector_len,
                        uint32_t c2_len)
{
    uint8_t sub[SUB_RAW];

    /* Only a raw-P-W record is laid out AUDIO + c2 + 96. Called on anything
     * else (formatted Q is 16 bytes) the 96-byte moves would cross into the
     * next record, so refuse by doing nothing. */
    if (sector_len != AUDIO + c2_len + SUB_RAW)
        return;
    for (uint32_t s = 0; s < nsec; s++) {
        uint8_t *rec = buf + (size_t)s * sector_len + AUDIO;

        memcpy(sub, rec, SUB_RAW);
        memmove(rec, rec + SUB_RAW, c2_len);
        memcpy(rec + c2_len, sub, SUB_RAW);
    }
}
