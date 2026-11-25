/* accudisc_read_policy (0.58.0): what a read is FOR -> the request that does it.
 *
 * Revision 2 (0.59.0, REFERENCE_VERIFY added; revision 1 was 0.58.0) of the table in accudisc.h, which is the contract; this file only
 * implements it. Pure: no device, no I/O. Everything is validated before
 * anything is written, so a refused intent leaves out and info exactly as the
 * caller had them.
 *
 * Settled with cdda2img: their 274 (the table, and the catch that the head far
 * read must be decided from the intent's own range), their 278 (shift_search
 * waits for the hardware A/B), our 2026-11-25k/l/y. */

#include <stddef.h>
#include <string.h>

#include "../internal.h"
#include "far.h"

/* The last byte of accudisc_read_req the policy writes, plus one. */
#define ADSC_POLICY_OUT_MIN \
    (offsetof(accudisc_read_req, budget_cmds) + sizeof(uint32_t))

int accudisc_read_policy(const accudisc_read_intent *in, accudisc_read_req *out,
                         accudisc_read_policy_info *info)
{
    accudisc_read_intent it;
    size_t info_n;
    int rc;

    if (!in || !out || !info)
        return ACCUDISC_ERR_INVAL;
    rc = adsc_abi_import(&it, sizeof(it), in, in->size);
    if (rc != ACCUDISC_OK)
        return rc;
    if (out->size < ADSC_POLICY_OUT_MIN || out->size > sizeof(*out))
        return ACCUDISC_ERR_ABI;
    rc = adsc_abi_export(info->size, sizeof(*info), &info_n);
    if (rc != ACCUDISC_OK)
        return rc;
    if (info_n < sizeof(*info)) /* every field is written; none may be lost */
        return ACCUDISC_ERR_ABI;

    /* ---- the intent ---- */
    switch (it.purpose) {
    case ACCUDISC_PURPOSE_CAPTURE:
    case ACCUDISC_PURPOSE_GATED_REREAD:
    case ACCUDISC_PURPOSE_SPAN_RESCUE:
    case ACCUDISC_PURPOSE_REFERENCE_VERIFY: /* revision 2 */
        break;
    default:
        return ACCUDISC_ERR_INVAL; /* 0 included: an uninitialised intent */
    }
    if (it.count == 0)
        return ACCUDISC_ERR_INVAL;
    if (it.readable_hi && it.readable_lo >= it.readable_hi)
        return ACCUDISC_ERR_INVAL;
    if (it.n_admitted > 16)
        return ACCUDISC_ERR_INVAL;

    /* A sorted copy of the admitted rungs: the caller's order is irrelevant,
     * and a zero or a duplicate is not a rung. */
    uint16_t adm[16];
    unsigned na = it.n_admitted;
    for (unsigned i = 0; i < na; i++) {
        uint16_t v = it.admitted_x[i];
        unsigned j = i;

        if (v == 0)
            return ACCUDISC_ERR_INVAL;
        while (j > 0 && adm[j - 1] > v) {
            adm[j] = adm[j - 1];
            j--;
        }
        if (j > 0 && adm[j - 1] == v)
            return ACCUDISC_ERR_INVAL;
        adm[j] = v;
    }
    int speed_admitted = 0;
    for (unsigned i = 0; i < na; i++)
        speed_admitted |= adm[i] == it.speed_x;
    if (it.speed_x && !speed_admitted)
        return ACCUDISC_ERR_INVAL;

    int rescue = it.purpose == ACCUDISC_PURPOSE_SPAN_RESCUE;
    if (rescue && (!it.speed_x || !it.c2_usable))
        return ACCUDISC_ERR_INVAL;

    /* REFERENCE_VERIFY picks its own rung: the NEAREST different one, the
     * highest admitted below the reference's honoured speed, else the lowest
     * above; never the reference's own nor exclude_speed_x. Nearest because it
     * buys a second independent read at close to the capture's cost; NOT
     * because slower is more reliable (Keith, 2026-10-22: it was not). */
    int refv = it.purpose == ACCUDISC_PURPOSE_REFERENCE_VERIFY;
    uint16_t speed = it.speed_x;
    int ref_above = 0;
    if (refv) {
        if (it.speed_x || !it.ref_speed_x)
            return ACCUDISC_ERR_INVAL;
        speed = 0;
        for (unsigned i = 0; i < na; i++)
            if (adm[i] < it.ref_speed_x && adm[i] != it.exclude_speed_x)
                speed = adm[i]; /* ascending: the last one below is highest */
        for (unsigned i = 0; i < na && !speed; i++)
            if (adm[i] > it.ref_speed_x && adm[i] != it.exclude_speed_x) {
                speed = adm[i];
                ref_above = 1;
            }
        if (!speed)
            return ACCUDISC_ERR_INVAL; /* no rung but the excluded ones */
    }

    /* ---- revision 1 ---- */
    accudisc_read_policy_info pi;
    memset(&pi, 0, sizeof(pi));
    pi.size = info->size;
    pi.revision = ACCUDISC_POLICY_REVISION;

    uint8_t c2 = it.c2_usable ? ACCUDISC_C2_PTRS : ACCUDISC_C2_NONE;
    if (!it.c2_usable)
        pi.reasons |= ACCUDISC_POLICY_R_NO_C2;
    if (!it.c2_usable && refv)
        pi.reasons |= ACCUDISC_POLICY_R_NO_C2_THIRD_PASS;
    if (ref_above)
        pi.reasons |= ACCUDISC_POLICY_R_REF_RUNG_ABOVE;
    /* sub NONE for the re-reads: on RAW the Q-position rescue would add far
     * reads, and REFERENCE_VERIFY promises one */
    uint8_t sub = it.purpose == ACCUDISC_PURPOSE_GATED_REREAD || refv
                      ? ACCUDISC_SUB_NONE
                      : ACCUDISC_SUB_RAW;

    /* The head far read, by the engine's own rule (far.h). */
    uint8_t defeat = 0;
    if (it.purpose != ACCUDISC_PURPOSE_CAPTURE) {
        uint32_t away;

        if (!it.readable_hi)
            pi.reasons |= ACCUDISC_POLICY_R_READABLE_UNKNOWN;
        if (adsc_far_head(it.lba, it.count, it.readable_lo, it.readable_hi,
                          &away) == ADSC_FAR_NONE)
            pi.reasons |= ACCUDISC_POLICY_R_DEFEAT_NO_TARGET;
        else
            defeat = 1;
    }
    if (it.purpose == ACCUDISC_PURPOSE_GATED_REREAD || refv)
        pi.reasons |= ACCUDISC_POLICY_R_ENGINE_RECOVERY_PENDING_AB;

    unsigned ladder_len = 0;
    if (rescue) {
        pi.reasons |= ACCUDISC_POLICY_R_C2_RESCUE_KEPT_OFF |
                      ACCUDISC_POLICY_R_SHIFT_SEARCH_PENDING_AB;
        for (unsigned i = na; i-- > 0;) /* below speed_x, descending */
            if (adm[i] < it.speed_x)
                pi.ladder[ladder_len++] = adm[i];
        if (!ladder_len) {
            for (unsigned i = 0; i < na && !ladder_len; i++)
                if (adm[i] > it.speed_x) /* the next one up */
                    pi.ladder[ladder_len++] = adm[i];
            pi.reasons |= ladder_len ? ACCUDISC_POLICY_R_LADDER_NEXT_UP
                                     : ACCUDISC_POLICY_R_NO_LADDER;
        }
    }
    if (it.drive_slips == 2)
        pi.reasons |= ACCUDISC_POLICY_R_SLIPS_KNOWN_RELATIVE_ONLY;

    /* ---- write: the policy-owned fields, and only those ---- */
    memcpy(info, &pi, info_n);
    out->lba = it.lba;
    out->count = it.count;
    out->c2 = c2;
    out->sub = sub;
    out->any_type = 0;
    out->retries = 0;
    out->chunk_sectors = 0;
    out->speed_x = speed;
    out->c2_retries = 0;
    out->verify_passes = rescue ? 2 : 0;
    out->overlap_sectors = 0;
    out->speed_ladder = ladder_len ? info->ladder : NULL;
    out->ladder_len = (uint8_t)ladder_len;
    out->c2_witness = 0;
    out->cache_defeat = defeat;
    out->readable_lo = it.readable_lo;
    out->readable_hi = it.readable_hi;
    out->rescue_by_rung = rescue ? 1 : 0;
    out->shift_search = 0;
    out->budget_speed_changes = it.budget_speed_changes;
    out->budget_cmds = it.budget_cmds;
    return ACCUDISC_OK;
}
