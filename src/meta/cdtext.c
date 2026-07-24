/* CD-Text decode: 18-byte packs from READ TOC format 5.
 *
 * Pack: [0] type, [1] track (bit 7 = extension), [2] sequence,
 * [3] block/charpos (bit 7 = double-byte chars, bits 6-4 block), [4..15]
 * text payload, [16..17] CRC (X.25, complemented).
 *
 * Payload of one type is a continuous stream of NUL-terminated strings. A
 * string of just TAB means "same as the previous track". Bytes are copied
 * through verbatim — no character-set interpretation (see header). v0: block 0,
 * single-byte packs.
 *
 * ATTRIBUTING STRINGS TO TRACKS — do not "simplify" this to counting NULs.
 * A disc may omit a track's string entirely, with no empty-string placeholder:
 * the stream runs ...\0<track N-1's string>\0<track N+1's string>\0... So the
 * n-th string is NOT the n-th track, and a decoder that counts separators from
 * the first pack's track number silently shifts every later string by one and
 * drops the last. Pack byte [1] is the authority: it states which track's
 * string is in progress at that pack's FIRST payload byte. It must therefore be
 * honoured on EVERY pack — including one that lands mid-string, which is where
 * a real gap shows up (ABBA "Gold" omits track 13's title; the correction
 * 13 -> 14 arrives in a pack whose first bytes continue track 14's title).
 *
 * That is also why a completed string is buffered and committed on its NUL
 * rather than written into the destination field as it arrives: a mid-string
 * resync has to be able to change where the string lands.
 */

#include <stdlib.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "../cdda/crc16.h"

#define PACK_LEN 18

struct assembler {
    accudisc_cdtext *text;
    unsigned type;
    int track;    /* track the in-progress string belongs to; -1 = no stream */
    unsigned pos; /* length of buf */
    char buf[ACCUDISC_TEXT_MAX]; /* string under construction, committed on NUL */
};

static char *field_for(accudisc_cdtext *t, unsigned type, int track)
{
    accudisc_cdtext_strings *s;

    if (track < 0 || track > 99)
        return NULL;
    s = track == 0 ? &t->album : &t->track[track];
    switch (type) {
    case 0x80: return s->title;
    case 0x81: return s->performer;
    case 0x82: return s->songwriter;
    case 0x8e: return s->code;
    default:   return NULL;
    }
}

static void put_char(struct assembler *a, char c)
{
    if (c != '\0') {
        if (a->pos < ACCUDISC_TEXT_MAX - 1)
            a->buf[a->pos++] = c;
        return;
    }

    /* String complete. Commit it to whichever track the pack headers settled
     * on — which may have been corrected while these bytes were arriving. */
    a->buf[a->pos] = '\0';

    char *dst = field_for(a->text, a->type, a->track);
    if (dst) {
        if (a->pos == 1 && a->buf[0] == '\t' && a->track >= 1) {
            /* TAB alone means "same as the previous track". */
            const char *prev = field_for(a->text, a->type, a->track - 1);
            if (prev)
                memcpy(dst, prev, ACCUDISC_TEXT_MAX);
            else
                dst[0] = '\0';
        } else {
            memcpy(dst, a->buf, a->pos + 1);
        }
    }
    /* Provisional guess for the next string; the next pack header corrects it. */
    a->track++;
    a->pos = 0;
}

int accudisc_cdtext_decode(const uint8_t *raw, uint32_t len,
                           accudisc_cdtext **out)
{
    if (!raw || !out)
        return ACCUDISC_ERR_INVAL;
    if (len < 4 + PACK_LEN)
        return ACCUDISC_ERR_SHORT;

    accudisc_cdtext *text = calloc(1, sizeof(*text));
    if (!text)
        return ACCUDISC_ERR_NOMEM;

    /* One assembler per pack type keeps interleaved type runs independent. */
    struct assembler asm80 = { .text = text, .type = 0x80, .track = -1 };
    struct assembler asm81 = { .text = text, .type = 0x81, .track = -1 };
    struct assembler asm82 = { .text = text, .type = 0x82, .track = -1 };
    struct assembler asm8e = { .text = text, .type = 0x8e, .track = -1 };
    unsigned used = 0;

    for (uint32_t off = 4; off + PACK_LEN <= len; off += PACK_LEN) {
        const uint8_t *p = raw + off;
        struct assembler *a;

        uint16_t want = (uint16_t)~(((uint16_t)p[16] << 8) | p[17]);
        if (adsc_crc16(p, 16) != want)
            continue; /* corrupt pack: skip, never guess */
        if (p[1] & 0x80)
            continue; /* extension packs: out of v0 scope */
        if (p[3] & 0x80)
            continue; /* double-byte character packs: out of v0 scope */
        if ((p[3] >> 4) & 0x07)
            continue; /* block > 0 (other languages): out of v0 scope */

        switch (p[0]) {
        case 0x80: a = &asm80; break;
        case 0x81: a = &asm81; break;
        case 0x82: a = &asm82; break;
        case 0x8e: a = &asm8e; break;
        default:   continue;
        }
        /* Resync on EVERY pack, not just the first: byte [1] states which
         * track's string is in progress here, and it is the only signal that
         * survives an omitted string. See the header comment. */
        a->track = (int)(p[1] & 0x7f);
        for (unsigned i = 4; i < 16; i++)
            put_char(a, (char)p[i]);
        used++;
    }

    if (!used) {
        free(text);
        return ACCUDISC_ERR_SHORT;
    }
    *out = text;
    return ACCUDISC_OK;
}
