/* Drive offset lookup — the single portal for offset data.
 *
 * The table is compiled by tools/gen_offsets.py from the two live primary
 * sources (REDUMP via redumper, and AccurateRip), both factual user-submitted
 * measurement data — attributed in docs/reference/ATTRIBUTION.md.
 * Nothing here reaches a network: the merge happens on the development cycle
 * and its output is committed.
 *
 * Matching: INQUIRY vendor/product with whitespace runs collapsed, since drives
 * pad the fixed INQUIRY fields ("DVDR   PX-716A" vs "DVDR PX-716A"), and
 * CASE-FOLDED — the table is generated upper-cased and the query is upper-cased
 * to meet it. Vendors are not consistent with themselves ("AOpen"/"AOPEN"), so
 * a case-sensitive compare answers only for the casing the firmware happened to
 * use and silently misses the other. What the lookup does NOT do is alias:
 * HL-DT-ST and LG ELECTRONICS are one company and two INQUIRY strings, and the
 * generator emits a row for each rather than the lookup asserting the identity.
 *
 * The `sources` bitmask reports PRESENCE in each table, not corroboration by
 * independent parties. The tables are not merely correlated: REDUMP's offset
 * table IS AccurateRip's list, imported once in 2022 and frozen. The evidence
 * is on accudisc_offset_info.sources in the public header.
 *
 * A value REDUMP holds that AccurateRip has since changed or removed is that
 * one source's withdrawn draft, and the generator drops it rather than
 * reporting a conflict — each deletion is named in offsets_db.inc.
 */

#include <ctype.h>
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

/* The table is a parameter so a test can substitute a small one. The generated
 * corpus currently has ZERO conflicting keys, which makes the n > 1 branch of
 * accudisc_offset_for_inquiry unreachable from shipped data and therefore
 * impossible to exercise against the real table — live code with no guard.
 * tests/test_offsets_ambiguous.c compiles THIS FILE against a fixture instead
 * of a copy of it, so what it asserts is the shipped implementation.
 *
 * Default only. Nothing but that test ever defines it, and the library builds
 * exactly as before. */
#ifndef ADSC_OFFSETS_DB
#  define ADSC_OFFSETS_DB "offsets_db.inc"
#endif

static const struct offset_entry offsets[] = {
#include ADSC_OFFSETS_DB
};

#define OFFSETS_N (sizeof(offsets) / sizeof(offsets[0]))

/* The INQUIRY matching rule, one implementation: collapse whitespace runs to
 * single spaces, trim ends, and UPPER-CASE.
 *
 * Case folding is deliberate and is the whole rule, not a convenience. Vendors
 * are not consistent about it — this corpus carries "AOpen" and "AOPEN",
 * "Plextor" and "PLEXTOR", "hp" and "HP" for the same companies — so a
 * case-sensitive compare turns one drive into two keys and answers only for
 * whichever casing the firmware happened to use. The generator emits the table
 * already upper-cased; folding here as well makes that a property the lookup
 * enforces rather than one it trusts the generator to have preserved.
 *
 * Verified lossless before the change: of 5888 table rows, ZERO pairs differ
 * only by case, so folding collides nothing that was previously distinct.
 *
 * UNDERSCORES ARE NOT FOLDED HERE, and that is a decision rather than an
 * omission. The GENERATOR folds underscore to space when it builds its join key,
 * so "DVDRAM_GHA2N" and "DVDRAM GHA2N" pool their submissions into one drive's
 * evidence — but it then emits BOTH spellings as their own rows, and this
 * lookup matches each literally. The asymmetry is the same one that governs
 * VENDOR_ALIAS: a build-time key may assert that two strings name one drive; a
 * runtime lookup may only answer for the string the firmware actually reported.
 * Folding here as well would answer for spellings no source ever recorded.
 *
 * ASCII-only by intent. INQUIRY fields are single-byte and this must not depend
 * on locale — toupper() with a negative char is undefined, hence the cast. */
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
        dst[o++] = (char)toupper((unsigned char)*src);
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
