/* READ TOC format 2 ("full TOC") parse: the lead-in's raw session entries.
 * Response: 2-byte length, first/last session, then 11-byte descriptors
 * (session, adr/ctrl, tno, point, min/sec/frame, zero, pmin/psec/pframe). */

#include <string.h>

#include <accudisc/accudisc.h>

int accudisc_fulltoc_parse(const uint8_t *raw, uint32_t len,
                           accudisc_fulltoc *out)
{
    if (!raw || !out)
        return ACCUDISC_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    if (len < 4)
        return ACCUDISC_ERR_SHORT;

    uint32_t end = (uint32_t)(((unsigned)raw[0] << 8) | raw[1]) + 2;
    if (end > len)
        end = len;
    out->first_session = raw[2];
    out->last_session = raw[3];

    for (uint32_t off = 4; off + 11 <= end; off += 11) {
        const uint8_t *d = raw + off;
        accudisc_fulltoc_entry *e;

        if (out->entry_count >=
            sizeof(out->entries) / sizeof(out->entries[0]))
            return ACCUDISC_ERR_SHORT; /* malformed: more than a CD can hold */
        e = &out->entries[out->entry_count++];
        e->session = d[0];
        e->adr_ctrl = d[1];
        /* d[2] is TNO, always 0 in the lead-in */
        e->point = d[3];
        e->min = d[4];
        e->sec = d[5];
        e->frame = d[6];
        /* d[7] is ZERO */
        e->pmin = d[8];
        e->psec = d[9];
        e->pframe = d[10];
    }
    return out->entry_count ? ACCUDISC_OK : ACCUDISC_ERR_SHORT;
}
