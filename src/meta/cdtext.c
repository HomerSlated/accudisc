/* CD-Text decode: 18-byte packs from READ TOC format 5.
 *
 * Pack: [0] type, [1] track (bit 7 = extension), [2] sequence,
 * [3] block/charpos (bit 7 = double-byte chars, bits 6-4 block), [4..15]
 * text payload, [16..17] CRC (X.25, complemented).
 *
 * Payload of one type is a continuous stream of NUL-terminated strings, one
 * per track, starting at the pack's track number. A string of just TAB means
 * "same as the previous track". Bytes are copied through verbatim — no
 * character-set interpretation (see header). v0: block 0, single-byte packs.
 */

#include <stdlib.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "../cdda/crc16.h"

#define PACK_LEN 18

struct assembler {
    accudisc_cdtext *text;
    unsigned type;
    int track;    /* string currently being built; -1 = no stream yet */
    unsigned pos; /* write position within the current string */
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
    char *dst = field_for(a->text, a->type, a->track);

    if (!dst)
        return;
    if (c == '\0') {
        /* String complete: TAB alone means "same as previous track". */
        if (a->pos == 1 && dst[0] == '\t' && a->track >= 1) {
            const char *prev = field_for(a->text, a->type, a->track - 1);
            if (prev)
                memcpy(dst, prev, ACCUDISC_TEXT_MAX);
            else
                dst[0] = '\0';
        }
        a->track++;
        a->pos = 0;
        return;
    }
    if (a->pos < ACCUDISC_TEXT_MAX - 1) {
        dst[a->pos++] = c;
        dst[a->pos] = '\0';
    }
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
    struct assembler asm80 = {text, 0x80, -1, 0};
    struct assembler asm81 = {text, 0x81, -1, 0};
    struct assembler asm82 = {text, 0x82, -1, 0};
    struct assembler asm8e = {text, 0x8e, -1, 0};
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
        /* First pack of a type anchors the track counter; later packs
         * continue the stream (their track field restates where the
         * stream currently is). */
        if (a->track < 0) {
            a->track = p[1] & 0x7f;
            a->pos = 0;
        }
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
