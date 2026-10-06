/* DAO write-parameters setup (recording engine, phase 1 slice 1).
 *
 * Read the write-parameters mode page (0x05), configure it for Disc-At-Once
 * (Session-At-Once) audio recording, and select it back. This only programs
 * the drive's write registers — it does NOT touch the disc, so it is safe and
 * fully reversible (a power cycle resets the page; an eject may not — see the
 * round-trip note in write.h). Field layout
 * and the variant handling follow cdrdao's GenericMMC::setWriteParameters
 * (private/code/cdrdao/dao/GenericMMC.cc); credited in docs/reference/ATTRIBUTION.md.
 */

#include <string.h>

#include "../mmc/mmc.h"
#include "write.h"

/* Copy the page (from its page-code byte) out of a mode-parameter buffer. The
 * length is the page's own claim (byte 1 + 2), clamped to what was transferred
 * and to the capture buffer -- drive data, so never trusted as a bound. */
static uint32_t wp_capture(uint8_t *dst, const uint8_t *buf, uint32_t len,
                           uint32_t po)
{
    uint32_t n = (uint32_t)buf[po + 1] + 2u;

    if (n > len - po)
        n = len - po;
    if (n > ADSC_WPARAMS_PAGE_MAX)
        n = ADSC_WPARAMS_PAGE_MAX;
    memcpy(dst, buf + po, n);
    return n;
}

/* The one MODE SELECT sequence, shared by the burn and by the round trip so
 * the round trip tests the bytes the burn sends rather than a second copy of
 * them. With rt == NULL this is exactly the burn's behaviour. */
static int wp_program(struct accudisc_device *dev,
                      const struct adsc_write_params *wp,
                      struct adsc_wparams_roundtrip *rt)
{
    uint8_t buf[64];
    uint32_t len = 0, po = 0;
    int rc;

    rc = adsc_mmc_mode_sense10(dev, ADSC_MODE_WRITE_PARAMS, buf,
                               sizeof(buf), &len, &po);
    if (rc != ACCUDISC_OK)
        return rc;
    if (po + 9 > len)             /* need bytes 0..8 of the page */
        return ACCUDISC_ERR_SHORT;

    uint8_t *p = buf + po;

    if (rt)
        rt->before_len = wp_capture(rt->before, buf, len, po);

    /* byte 2: [BUFE(0x40)][Test Write(0x10)][Write Type(low 4)]. */
    p[2] = (uint8_t)(p[2] & 0xe0);      /* keep BUFE/LS-V, clear write type */
    p[2] |= 0x02;                       /* write type = Session-at-once (DAO) */
    if (wp->simulate)
        p[2] |= 0x10;                   /* test write: exercise path, no laser */
    if (wp->burnproof)
        p[2] |= 0x40;                   /* BURN-Proof buffer-underrun protect */

    /* byte 3: multisession bits 7-6. Single session, next not allowed. */
    p[3] = (uint8_t)(p[3] & 0x3f);

    /* byte 4: data block type (low nibble). Audio DAO uses raw 2352 (0), or
     * raw + P-W subchannel 2448 (3) when CD-Text must go in the lead-in. */
    p[4] = (uint8_t)((p[4] & 0xf0) | (wp->cdtext ? 0x03 : 0x00));

    /* byte 8: session format. 0x00 = CD-DA / CD-ROM. */
    p[8] = 0x00;

    rc = adsc_mmc_mode_select10(dev, buf, len, po);
    if (rc == ACCUDISC_OK || !wp->cdtext) {
        if (rc == ACCUDISC_OK && wp->cdtext)
            adsc_dev_log(dev, "cdtext: write params accepted with P-W subchannel "
                              "(data block type 3)");
        /* Captured AFTER the call: mode_select10 clears the PS bit and the
         * header length in place, so this is what went down the wire. */
        if (rt)
            rt->sent_len = wp_capture(rt->sent, buf, len, po);
        return rc;
    }

    /* Some drives reject the page with data block type 3. cdrdao meets this
     * with a mode-page variant that simply omits it (its
     * WMP_VAR_CDTEXT_NO_DATA_BLOCK_TYPE) and still writes CD-Text, so retry
     * that way rather than failing the burn outright. But a drive that will
     * not take DBT 3 may not honour the P-W lead-in write at all, so this must
     * NOT be silent — a burn that drops the CD-Text and exits 0 is the exact
     * failure mode this project refuses. */
    adsc_dev_log(dev, "cdtext: WARNING drive rejected data block type 3 "
                      "(raw + P-W); retrying without it — CD-Text lead-in may "
                      "not be written on this drive");
    p[4] = (uint8_t)(p[4] & 0xf0);
    rc = adsc_mmc_mode_select10(dev, buf, len, po);
    if (rt)
        rt->sent_len = wp_capture(rt->sent, buf, len, po);
    return rc;
}

int adsc_write_set_params(struct accudisc_device *dev,
                          const struct adsc_write_params *wp)
{
    if (!dev || !wp)
        return ACCUDISC_ERR_INVAL;
    return wp_program(dev, wp, NULL);
}

int adsc_write_set_params_sent(struct accudisc_device *dev,
                               const struct adsc_write_params *wp,
                               uint8_t *sent, uint32_t *sent_len)
{
    struct adsc_wparams_roundtrip rt;
    int rc;

    if (!dev || !wp || !sent || !sent_len)
        return ACCUDISC_ERR_INVAL;
    memset(&rt, 0, sizeof(rt));
    rc = wp_program(dev, wp, &rt);
    memcpy(sent, rt.sent, ADSC_WPARAMS_PAGE_MAX);
    *sent_len = rt.sent_len;
    return rc;
}

/* Bytes of the page that decide what a burn does. Byte 0's top two bits (PS
 * and a reserved bit) are cleared on select and may come back set, so they are
 * never part of a comparison. */
static int wp_fields_equal(const uint8_t *a, const uint8_t *b)
{
    return ((a[2] ^ b[2]) & 0x5f) == 0 &&   /* BUFE, Test Write, write type */
           ((a[3] ^ b[3]) & 0xc0) == 0 &&   /* multisession */
           ((a[4] ^ b[4]) & 0x0f) == 0 &&   /* data block type */
           a[8] == b[8];                    /* session format */
}

static int wp_pages_equal(const uint8_t *a, uint32_t alen, const uint8_t *b,
                          uint32_t blen)
{
    if (alen != blen || alen < 1)
        return 0;
    if ((a[0] ^ b[0]) & 0x3f)
        return 0;
    return memcmp(a + 1, b + 1, alen - 1) == 0;
}

int adsc_write_params_check(struct accudisc_device *dev, const uint8_t *sent,
                            uint32_t sent_len, struct adsc_wparams_check *out)
{
    uint8_t buf[64];
    uint32_t len = 0, po = 0;
    int rc;

    /* 9 bytes of the sent page are compared field by field below. */
    if (!dev || !sent || !out || sent_len < 9 ||
        sent_len > ADSC_WPARAMS_PAGE_MAX)
        return ACCUDISC_ERR_INVAL;
    memset(out, 0, sizeof(*out));

    rc = adsc_mmc_mode_sense10(dev, ADSC_MODE_WRITE_PARAMS, buf,
                               sizeof(buf), &len, &po);
    if (rc != ACCUDISC_OK)
        return rc;
    if (po + 9 > len)
        return ACCUDISC_ERR_SHORT;
    out->held_len = wp_capture(out->held, buf, len, po);

    const uint8_t *h = out->held;

    out->bad_write_type     = (uint8_t)(((sent[2] ^ h[2]) & 0x0f) != 0);
    out->bad_test_write     = (uint8_t)(((sent[2] ^ h[2]) & 0x10) != 0);
    out->bad_bufe           = (uint8_t)(((sent[2] ^ h[2]) & 0x40) != 0);
    out->bad_multisession   = (uint8_t)(((sent[3] ^ h[3]) & 0xc0) != 0);
    out->bad_block_type     = (uint8_t)(((sent[4] ^ h[4]) & 0x0f) != 0);
    out->bad_session_format = (uint8_t)(sent[8] != h[8]);
    out->fields_ok = (uint8_t)wp_fields_equal(sent, h);

    for (uint32_t i = 0; i < sent_len && i < out->held_len; i++)
        if ((sent[i] ^ h[i]) & (i == 0 ? 0x3f : 0xff))
            out->diff_bytes++;
    if (sent_len != out->held_len)
        out->diff_bytes++;
    return ACCUDISC_OK;
}

int adsc_write_params_roundtrip(struct accudisc_device *dev,
                                const struct adsc_write_params *wp,
                                struct adsc_wparams_roundtrip *rt)
{
    uint8_t buf[64];
    uint32_t len = 0, po = 0;
    int rc;

    if (!dev || !wp || !rt)
        return ACCUDISC_ERR_INVAL;
    memset(rt, 0, sizeof(*rt));

    rt->stage = ADSC_WPRT_STAGE_SELECT;
    rc = wp_program(dev, wp, rt);
    if (rc != ACCUDISC_OK)
        return rc;

    rt->stage = ADSC_WPRT_STAGE_READBACK;
    rc = adsc_mmc_mode_sense10(dev, ADSC_MODE_WRITE_PARAMS, buf,
                               sizeof(buf), &len, &po);
    if (rc != ACCUDISC_OK)
        return rc;
    if (po + 9 > len)
        return ACCUDISC_ERR_SHORT;
    rt->after_len = wp_capture(rt->after, buf, len, po);
    rt->stage = ADSC_WPRT_STAGE_DONE;

    /* 9 bytes of each were required above (and by wp_program), so the field
     * comparisons below never read past a capture. */
    rt->fields_ok = (uint8_t)wp_fields_equal(rt->sent, rt->after);
    rt->page_ok = (uint8_t)wp_pages_equal(rt->sent, rt->sent_len, rt->after,
                                          rt->after_len);
    for (uint32_t i = 0; i < rt->sent_len && i < rt->after_len; i++)
        if ((rt->sent[i] ^ rt->after[i]) & (i == 0 ? 0x3f : 0xff))
            rt->diff_bytes++;
    if (rt->sent_len != rt->after_len)
        rt->diff_bytes++;

    /* WHAT THE REQUEST COULD PROVE. If the page already held the values we
     * sent, a drive that applied the select and a drive that dropped it read
     * back identically -- the comparison is correct and says nothing. That is
     * reported, never folded into "ok": it is the caller's cue to send a
     * request that differs (the probe toggles Test Write for this reason). */
    rt->changed = (uint8_t)!wp_pages_equal(rt->before, rt->before_len,
                                           rt->sent, rt->sent_len);
    rt->ignored = (uint8_t)(rt->changed && !rt->page_ok &&
                            wp_pages_equal(rt->before, rt->before_len,
                                           rt->after, rt->after_len));
    return ACCUDISC_OK;
}

int adsc_write_get_params(struct accudisc_device *dev,
                          struct adsc_write_params *out)
{
    uint8_t buf[64];
    uint32_t len = 0, po = 0;
    int rc;

    if (!dev || !out)
        return ACCUDISC_ERR_INVAL;

    rc = adsc_mmc_mode_sense10(dev, ADSC_MODE_WRITE_PARAMS, buf,
                               sizeof(buf), &len, &po);
    if (rc != ACCUDISC_OK)
        return rc;
    if (po + 9 > len)
        return ACCUDISC_ERR_SHORT;

    const uint8_t *p = buf + po;
    memset(out, 0, sizeof(*out));
    out->write_type = (uint8_t)(p[2] & 0x0f);
    out->simulate   = (p[2] & 0x10) ? 1 : 0;
    out->burnproof  = (p[2] & 0x40) ? 1 : 0;
    out->cdtext     = ((p[4] & 0x0f) == 0x03) ? 1 : 0;
    return ACCUDISC_OK;
}

int adsc_write_params_restore(struct accudisc_device *dev,
                              const uint8_t *page, uint32_t page_len)
{
    uint8_t buf[64];
    uint32_t len = 0, po = 0;
    int rc;

    if (!dev || !page || page_len < 2)
        return ACCUDISC_ERR_INVAL;

    rc = adsc_mmc_mode_sense10(dev, ADSC_MODE_WRITE_PARAMS, buf,
                               sizeof(buf), &len, &po);
    if (rc != ACCUDISC_OK)
        return rc;
    /* Only a page of the shape the drive has now can go back: a capture that
     * was clamped shorter than the page would select a truncated page. */
    if (po + page_len > len || buf[po + 1] != page[1] ||
        page_len != (uint32_t)page[1] + 2u)
        return ACCUDISC_ERR_SHORT;
    memcpy(buf + po, page, page_len);
    return adsc_mmc_mode_select10(dev, buf, len, po);
}
