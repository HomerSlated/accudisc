/* Drive read-offset lookup.
 *
 * Seed table only: a handful of drives relevant to this project, entered as
 * facts (offset values are AccurateRip-derived public data, cross-checked
 * against redumper's table). The full ~4600-entry import from
 * reference/redumper/offsets.ixx is pending a licensing review — see
 * docs/initial-assessment.md.
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
    { "PLEXTOR", "CD-R PREMIUM",  +30 },
    { "PLEXTOR", "CD-R PREMIUM2", +30 },
    { "PLEXTOR", "DVDR PX-708A",  +30 },
    { "PLEXTOR", "DVDR PX-712A",  +30 },
    { "PLEXTOR", "DVDR PX-716A",  +30 },
    { "PLEXTOR", "DVDR PX-716AL", +30 },
    { "PLEXTOR", "DVDR PX-755A",  +30 },
    { "PLEXTOR", "DVDR PX-760A",  +30 },
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
