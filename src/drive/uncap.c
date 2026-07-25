/* Vendor read-speed uncap: is it on?
 *
 * The uncap (Plextor calls it SpeedRead) corrupts the Q subchannel on inner and
 * mid tracks — measured 0% Q-CRC there, with the audio main channel untouched
 * and no error reported anywhere (drivers/plextor/FEATURES.md). A read that
 * captures subchannel while it is on returns confidently wrong metadata. So the
 * read engine wants to know, and the difficulty is that the honest answer is
 * sometimes "we cannot tell":
 *
 *   - accudisc_speed_uncap_get needs an attached vendor driver;
 *   - the setting is persistent DRIVE state, so a previous session, or another
 *     tool entirely, can have left it on before this handle was ever opened.
 *
 * Hence a four-valued answer rather than a boolean. The alternative — collapsing
 * "we inferred it" into "it is on" and refusing the read — would leave a caller
 * unable to capture subchannel at all on a drive we merely fail to recognise,
 * with no diagnosis available. Reporting beats guessing; see API_PLAN §9.
 */

#include <string.h>

#include "../internal.h"

/* Stock (un-uncapped) maximum CD read speed per drive model, in Nx.
 *
 * ENTRY RULE, and the reason this table is short: a row exists only where the
 * uncap transition has been observed in BOTH directions on that model — on
 * raising max_x, and off restoring it. That is what makes "max_x is at stock,
 * therefore the uncap is off" a deduction rather than a hope. A model we have
 * not tested that way does not get a guessed row; it resolves to UNKNOWN.
 *
 * This replaces a bare `max_x > 40` — which is this one drive's stock ceiling
 * promoted to a universal constant. Any drive whose stock ceiling is above 40x
 * would trip that test with the uncap off, and we own one drive, so we cannot
 * bound how many such drives exist.
 */
struct stock_ceiling {
    const char *vendor;
    const char *product;
    unsigned stock_x; /* mode page 2A maximum read speed with the uncap OFF */
};

static const struct stock_ceiling stock_ceilings[] = {
    /* PX-716A: SpeedRead ON flips page-2A max read 40x -> 48x (7056 -> 8467
     * kB/s), SET OFF restores 40x. Verified live, both directions, session 4 —
     * drivers/plextor/FEATURES.md feature 1. */
    { "PLEXTOR", "DVDR PX-716A", 40 },
};

/* Page 2A reports kB/s; 1x CD = 176 kB/s. Integer division is deliberate — we
 * compare against a whole-Nx ceiling and the drive's figure is approximate. */
#define KBPS_PER_X 176u

/* The table decision, split out from the I/O around it so it can be tested
 * against every model and speed without a drive attached — the same seam, and
 * the same reason, as cli/format.c. Do NOT give this an accudisc_device
 * parameter: the moment it can query the drive, it stops being testable.
 *
 * Returns LIKELY_ON / OFF for a known model, UNKNOWN for one we have not
 * verified or for max_x == 0 (nothing to judge). */
accudisc_uncap_state adsc_uncap_classify(const char *vendor,
                                         const char *product, unsigned max_x)
{
    char v[32], p[32], want_p[32];

    if (!vendor || !product || max_x == 0)
        return ACCUDISC_UNCAP_UNKNOWN;

    adsc_inquiry_normalize(vendor, v, sizeof(v));
    adsc_inquiry_normalize(product, p, sizeof(p));

    for (size_t i = 0; i < sizeof(stock_ceilings) / sizeof(stock_ceilings[0]);
         i++) {
        adsc_inquiry_normalize(stock_ceilings[i].product, want_p,
                               sizeof(want_p));
        if (strcmp(v, stock_ceilings[i].vendor) != 0 ||
            strcmp(p, want_p) != 0)
            continue;

        /* Above this model's verified stock ceiling: something lifted it, and
         * on every model in this table the only thing that does is the uncap.
         * Still an inference from a speed number, so it is reported as one. */
        return max_x > stock_ceilings[i].stock_x ? ACCUDISC_UNCAP_LIKELY_ON
                                                 : ACCUDISC_UNCAP_OFF;
    }
    return ACCUDISC_UNCAP_UNKNOWN; /* model not in the table: not "off" */
}

/* Sources 1 and 2 only — the two that can yield an authoritative answer, and
 * the two that cost no MODE SENSE. Split out because accudisc_read_cdda calls
 * this on every subchannel read: the read engine refuses only on an
 * authoritative ON, so computing the *inferred* state there would be a drive
 * command issued into the hot path for a value that cannot change the outcome.
 *
 * Yields ON, OFF, or UNKNOWN. Never LIKELY_ON — that is source 3's to give. */
accudisc_uncap_state adsc_uncap_authoritative(accudisc_device *dev)
{
    int on = 0;

    /* 1. We set it ourselves through this handle. No driver needed, no
     *    inference: whatever the drive was doing before, we know what we did.
     *    Deliberately survives accudisc_driver_detach — that is the point of
     *    it. Detaching a driver does not un-set a persistent drive setting. */
    if (dev->uncap_set != 0)
        return dev->uncap_set > 0 ? ACCUDISC_UNCAP_ON : ACCUDISC_UNCAP_OFF;

    /* 2. A driver is attached and will answer authoritatively. Returns
     *    ERR_UNSUPPORTED with no driver, which leaves us at UNKNOWN. */
    if (accudisc_speed_uncap_get(dev, &on) == ACCUDISC_OK)
        return on ? ACCUDISC_UNCAP_ON : ACCUDISC_UNCAP_OFF;

    return ACCUDISC_UNCAP_UNKNOWN;
}

int accudisc_speed_uncap_push(accudisc_device *dev, int on, int *prior_out)
{
    int prior = 0, rc;

    if (!dev || !prior_out)
        return ACCUDISC_ERR_INVAL;

    /* -1 until we have something worth restoring, so a caller that passes this
     * straight to pop() on an early error path gets a no-op rather than a
     * write of whatever was in its stack slot. */
    *prior_out = -1;

    rc = accudisc_speed_uncap_get(dev, &prior);
    if (rc != ACCUDISC_OK)
        return rc; /* never set what we cannot put back */

    /* Record before attempting the set, not after. A failed set may have
     * partially applied and we have no way to tell; restoring unnecessarily
     * costs one command, failing to restore costs the user's drive state. */
    *prior_out = prior;

    return accudisc_speed_uncap_set(dev, on);
}

int accudisc_speed_uncap_pop(accudisc_device *dev, int prior)
{
    if (!dev)
        return ACCUDISC_ERR_INVAL;
    if (prior < 0)
        return ACCUDISC_OK; /* nothing was pushed — pop only what you pushed */
    return accudisc_speed_uncap_set(dev, prior ? 1 : 0);
}

int accudisc_speed_uncap_probe(accudisc_device *dev,
                               accudisc_uncap_state *state, unsigned *max_x)
{
    unsigned max_kbps = 0, mx = 0;

    if (!dev || !state)
        return ACCUDISC_ERR_INVAL;

    if (max_x)
        *max_x = 0;
    *state = adsc_uncap_authoritative(dev);

    /* 3. Driver-free inference from the reported ceiling. accudisc_get_speed
     *    issues a fresh MODE SENSE(10) on every call rather than caching at
     *    open (src/device.c), which is what lets this see state a prior session
     *    left behind. Read it even when the state is already settled, because
     *    the caller asked for max_x. */
    if (accudisc_get_speed(dev, &max_kbps, NULL) == ACCUDISC_OK)
        mx = max_kbps / KBPS_PER_X;
    if (max_x)
        *max_x = mx;

    if (*state != ACCUDISC_UNCAP_UNKNOWN || mx == 0)
        return ACCUDISC_OK; /* already decided, or nothing to infer from */

    if (adsc_dev_identify(dev) != ACCUDISC_OK)
        return ACCUDISC_OK; /* stays UNKNOWN — an answer, not a failure */

    *state = adsc_uncap_classify(dev->id.vendor, dev->id.product, mx);
    return ACCUDISC_OK;
}
