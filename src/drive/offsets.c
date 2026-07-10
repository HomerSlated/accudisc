/* Drive read-offset lookup.
 *
 * The table is factual user-submitted data collected by the REDUMP Disc
 * Preservation Project (https://redump.org) — attributed as a courtesy in
 * the docs; regenerate with tools/gen_offsets.py when upstream grows.
 *
 * Matching: INQUIRY vendor/product with whitespace runs collapsed, since
 * drives pad the fixed INQUIRY fields ("DVDR   PX-716A" vs "DVDR PX-716A").
 */

#include <string.h>

#include "../internal.h"

struct offset_entry {
    const char *vendor;
    const char *product;
    int32_t samples;
};

static const struct offset_entry offsets[] = {
#include "offsets_db.inc"
};

/* Collapse whitespace runs to single spaces, trim ends. */
static void normalize(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    int in_space = 1; /* swallows leading spaces */

    for (; *src && o + 1 < cap; src++) {
        if (*src == ' ' || *src == '\t') {
            in_space = 1;
            continue;
        }
        if (in_space && o > 0)
            dst[o++] = ' ';
        in_space = 0;
        dst[o++] = *src;
    }
    dst[o] = '\0';
}

int accudisc_read_offset(accudisc_device *dev, int32_t *samples)
{
    char vendor[32], product[32], want_p[32];
    int rc;

    if (!dev || !samples)
        return ACCUDISC_ERR_INVAL;
    rc = adsc_dev_identify(dev);
    if (rc != ACCUDISC_OK)
        return rc;

    normalize(dev->id.vendor, vendor, sizeof(vendor));
    normalize(dev->id.product, product, sizeof(product));

    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        normalize(offsets[i].product, want_p, sizeof(want_p));
        if (strcmp(vendor, offsets[i].vendor) == 0 &&
            strcmp(product, want_p) == 0) {
            *samples = offsets[i].samples;
            return ACCUDISC_OK;
        }
    }
    return ACCUDISC_ERR_NOTFOUND;
}
