/* TOC parser tests against a synthetic READ TOC format-0 (LBA) response. */

#include <assert.h>
#include <string.h>

#include "toc/toc.h"

/* Build one 8-byte track descriptor. */
static void desc(uint8_t *p, uint8_t adr_ctrl, uint8_t track, uint32_t lba)
{
    memset(p, 0, 8);
    p[1] = adr_ctrl;
    p[2] = track;
    p[4] = (uint8_t)(lba >> 24);
    p[5] = (uint8_t)(lba >> 16);
    p[6] = (uint8_t)(lba >> 8);
    p[7] = (uint8_t)lba;
}

int main(void)
{
    /* Mixed-mode style disc: audio, audio, data, lead-out. */
    uint8_t buf[4 + 4 * 8];
    uint16_t data_len = sizeof(buf) - 2;

    buf[0] = (uint8_t)(data_len >> 8);
    buf[1] = (uint8_t)data_len;
    buf[2] = 1;  /* first track */
    buf[3] = 3;  /* last track */
    desc(buf + 4, 0x10, 1, 0);          /* audio (ctrl bit 2 clear) */
    desc(buf + 12, 0x10, 2, 15000);
    desc(buf + 20, 0x14, 3, 30000);     /* data (ctrl 0x4) */
    desc(buf + 28, 0x10, 0xAA, 45000);  /* lead-out */

    accudisc_toc toc;
    assert(adsc_toc_parse(buf, sizeof(buf), &toc) == ACCUDISC_OK);
    assert(toc.first_track == 1 && toc.last_track == 3);
    assert(toc.track_count == 3);
    assert(toc.leadout_lba == 45000);

    assert(toc.tracks[0].number == 1 && toc.tracks[0].lba == 0);
    assert(toc.tracks[0].sectors == 15000);
    assert(toc.tracks[1].sectors == 15000);
    assert(toc.tracks[2].sectors == 15000); /* last track: to lead-out */

    assert(ACCUDISC_TRACK_IS_AUDIO(&toc.tracks[0]));
    assert(ACCUDISC_TRACK_IS_AUDIO(&toc.tracks[1]));
    assert(!ACCUDISC_TRACK_IS_AUDIO(&toc.tracks[2]));

    /* No lead-out descriptor -> ERR_SHORT. */
    accudisc_toc bad;
    assert(adsc_toc_parse(buf, 4 + 3 * 8, &bad) == ACCUDISC_ERR_SHORT);

    /* Truncated header -> ERR_SHORT, no crash. */
    assert(adsc_toc_parse(buf, 3, &bad) == ACCUDISC_ERR_SHORT);

    return 0;
}
