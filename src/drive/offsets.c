/* Drive offset lookup — the single portal for offset data.
 *
 * The table is compiled by tools/gen_offsets.py from the two live primary
 * sources (REDUMP via redumper, and AccurateRip), both factual user-submitted
 * measurement data — attributed in docs/reference/ATTRIBUTION.md.
 * Nothing here reaches a network: the merge happens on the development cycle
 * and its output is committed.
 *
 * Matching: INQUIRY vendor/product with whitespace runs collapsed, since drives
 * pad the fixed INQUIRY fields ("DVDR   PX-716A" vs "DVDR PX-716A"). Matching is
 * CASE-SENSITIVE on purpose — the comparison is against bytes a drive actually
 * reported, not against a curated name. (The generator folds case, but only to
 * pool evidence across sources; it emits every distinct spelling it saw so
 * either firmware convention still matches here.)
 */

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "../internal.h"

struct offset_entry {
    const char *vendor;
    const char *product;
    int32_t  read_offset;
    uint16_t ar_submissions;
    uint8_t  ar_agree_pct;
    uint8_t  sources;        /* ACCUDISC_OFFSET_SRC_* */
    uint8_t  flags;          /* ACCUDISC_OFFSET_F_* */
};

static const struct offset_entry offsets[] = {
#include "offsets_db.inc"
};

#define OFFSETS_N (sizeof(offsets) / sizeof(offsets[0]))

/* Collapse whitespace runs to single spaces, trim ends. Shared with the other
 * INQUIRY-keyed tables in this module (see src/drive/uncap.c) — declared in
 * internal.h so the matching rule stays one implementation, not two that drift. */
void adsc_inquiry_normalize(const char *src, char *dst, size_t cap)
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

int accudisc_offset_for_inquiry(const char *vendor, const char *product,
                                accudisc_offset_info *out)
{
    char want_v[32], want_p[32], have_v[32], have_p[32];
    const struct offset_entry *first = NULL;
    unsigned n = 0;

    if (!vendor || !product || !out)
        return ACCUDISC_ERR_INVAL;
    if (out->size == 0 || out->size > sizeof(*out))
        return ACCUDISC_ERR_ABI;

    /* Zero everything the caller's struct covers, then set the sentinels. A
     * caller that ignores the return code must not find a usable-looking number
     * here: ACCUDISC_OFFSET_NONE is INT32_MIN precisely because 0 is a
     * legitimate offset for hundreds of real drives and would apply cleanly. */
    memset((char *)out + sizeof(out->size), 0, out->size - sizeof(out->size));
    out->read_offset = ACCUDISC_OFFSET_NONE;

    adsc_inquiry_normalize(vendor, want_v, sizeof(want_v));
    adsc_inquiry_normalize(product, want_p, sizeof(want_p));

    for (size_t i = 0; i < OFFSETS_N; i++) {
        adsc_inquiry_normalize(offsets[i].vendor, have_v, sizeof(have_v));
        adsc_inquiry_normalize(offsets[i].product, have_p, sizeof(have_p));
        if (strcmp(want_v, have_v) != 0 || strcmp(want_p, have_p) != 0)
            continue;

        if (!first)
            first = &offsets[i];
        if (n < ACCUDISC_OFFSET_MAX_VALUES) {
            out->values[n] = offsets[i].read_offset;
            out->value_sources[n] = offsets[i].sources;
        }
        n++;
        out->sources |= offsets[i].sources;
        out->flags |= offsets[i].flags;
        /* AccurateRip's confidence figures describe the value AR itself holds.
         * On a conflicting key they are cleared below rather than taken from
         * whichever row the scan happened to reach first. */
        if (n == 1) {
            out->ar_submissions = offsets[i].ar_submissions;
            out->ar_agree_pct = offsets[i].ar_agree_pct;
        }
    }

    if (!first)
        return ACCUDISC_ERR_NOTFOUND;

    out->n_values = (uint8_t)(n > ACCUDISC_OFFSET_MAX_VALUES
                                  ? ACCUDISC_OFFSET_MAX_VALUES : n);
    if (n > 1) {
        /* Conflicting sources. read_offset stays ACCUDISC_OFFSET_NONE; the
         * caller prints values[], picks one, and passes it back through its own
         * configuration. AccuDisc does not pick, and never applies. */
        out->ar_submissions = 0;
        out->ar_agree_pct = 0;
        return ACCUDISC_ERR_AMBIGUOUS;
    }

    out->read_offset = first->read_offset;
    return ACCUDISC_OK;
}

int accudisc_offset_for_device(accudisc_device *dev, accudisc_offset_info *out)
{
    int rc;

    if (!dev || !out)
        return ACCUDISC_ERR_INVAL;
    rc = adsc_dev_identify(dev);
    if (rc != ACCUDISC_OK)
        return rc;
    return accudisc_offset_for_inquiry(dev->id.vendor, dev->id.product, out);
}

int accudisc_read_offset(accudisc_device *dev, int32_t *samples)
{
    accudisc_offset_info info = ACCUDISC_OFFSET_INFO_INIT;
    int rc;

    if (!dev || !samples)
        return ACCUDISC_ERR_INVAL;
    /* Pre-0.10 contract, kept verbatim for callers compiled against it: a
     * single number or nothing. A key whose sources disagree has no single
     * number, so it reports as ERR_AMBIGUOUS rather than silently handing back
     * whichever row the table happened to list first — which is exactly what
     * this function used to do. */
    rc = accudisc_offset_for_device(dev, &info);
    if (rc != ACCUDISC_OK)
        return rc;
    *samples = info.read_offset;
    return ACCUDISC_OK;
}
