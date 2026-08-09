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

/* SOURCE 3 (INFERENCE) WAS REMOVED HERE — 0.8.0, Keith's ruling 2026-08-09.
 *
 * What stood here: a per-model `stock_ceilings[]` table and
 * `adsc_uncap_classify`, which compared mode page 2A's advertised maximum read
 * speed against a model's known stock ceiling and returned LIKELY_ON when it
 * was higher. It let us guess whether a vendor read-speed uncap was on without
 * a driver attached.
 *
 * Keith: "You should neither be querying nor returning a value for something
 * that is unreachable without a driver. And you certainly shouldn't be
 * inferring it. The existence, accessibility, and value of the SpeedRead
 * setting is completely irrelevant. You request a speed, and the governor tells
 * you what you can have. That is your authoritative data. You don't need to
 * test, guess, infer, or query anything else."
 *
 * The inference was unsound at its root, not merely imprecise. Page 2A reports
 * the REQUEST, not the governed throughput, so `max_x` above a stock ceiling
 * says a higher number was accepted into a register — never that the drive can
 * or will deliver it. On CD-DA it demonstrably will not: the governor caps
 * regardless, which our own throughput ladder shows unambiguously. So the
 * quantity being compared was not evidence about the drive's behaviour at all.
 *
 * Removing it also deletes a whole question rather than answering one. The
 * A-vs-B media-class ambiguity — whether the uncap lifts the reported ceiling
 * by media class or reports the data ceiling throughout — existed ONLY because
 * this comparison existed. No table, no boundary, no unvalidated `>`, and no
 * disc to go and buy.
 *
 * Sources 1 and 2 remain and are genuinely authoritative: we set it through
 * this handle, or an attached driver answers. With neither, the honest answer
 * is UNKNOWN, which the enum already carries and which callers already handle.
 */

/* Page 2A reports kB/s; 1x CD = 176 kB/s. */
#define KBPS_PER_X 176u

/* Sources 1 and 2 only — the two that can yield an authoritative answer, and
 * the two that cost no MODE SENSE. Split out because accudisc_read_cdda calls
 * this on every subchannel read: the read engine refuses only on an
 * authoritative ON, so computing the *inferred* state there would be a drive
 * command issued into the hot path for a value that cannot change the outcome.
 *
 * Yields ON, OFF, or UNKNOWN. Since 0.8.0 these are the ONLY sources, so this
 * is also what accudisc_speed_uncap_probe returns — the split between the two
 * entry points is now about cost (this one issues no MODE SENSE) rather than
 * about authority. */
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

    /* max_x is still reported, and is still worth reading — but it is now a
     * REPORTED FIGURE HANDED BACK VERBATIM, never an input to a verdict. It is
     * page 2A's advertised maximum, i.e. the largest request the drive will
     * accept, which is not a claim about what it will deliver. Callers wanting
     * deliverable rates want accudisc_probe_speed_ladder, which times reads.
     *
     * Deliberately read even when *state is already settled: the caller asked
     * for it. accudisc_get_speed issues a fresh MODE SENSE(10) rather than
     * caching at open (src/device.c). */
    if (accudisc_get_speed(dev, &max_kbps, NULL) == ACCUDISC_OK)
        mx = max_kbps / KBPS_PER_X;
    if (max_x)
        *max_x = mx;

    /* No third source. UNKNOWN stays UNKNOWN — see the note at the top of this
     * file. We do not guess at a setting only a driver can answer for. */
    return ACCUDISC_OK;
}
