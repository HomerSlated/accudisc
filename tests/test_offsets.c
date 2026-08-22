/* The drive read-offset portal.
 *
 * This table decides a number the caller applies to audio, and a wrong value is
 * SILENT — well-formed PCM, shifted. So the cases below are chosen for what
 * they would catch rather than for coverage: the whitespace rule that real
 * INQUIRY padding depends on, the ambiguity path that must not hand back a
 * usable-looking number, and the sentinel that makes "no answer" impossible to
 * mistake for an answer.
 */

#include <assert.h>
#include <string.h>

#include <accudisc/accudisc.h>

int main(void)
{
    accudisc_offset_info info = ACCUDISC_OFFSET_INFO_INIT;

    /* --- the drive this project is developed on ------------------------- */
    assert(accudisc_offset_for_inquiry("PLEXTOR", "DVDR PX-716A", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 30);
    /* Held by both collections, so both bits must be set — a presence flag that
     * over-claims is the failure this asserts against. */
    assert(info.sources == (ACCUDISC_OFFSET_SRC_REDUMP | ACCUDISC_OFFSET_SRC_AR));
    assert(info.ar_submissions > 1000);   /* 2781 at the time of writing */
    assert(info.ar_agree_pct == 100);
    assert(info.n_values == 1);
    assert((info.flags & ACCUDISC_OFFSET_F_CONFLICT) == 0);

    /* --- INQUIRY padding is not cosmetic --------------------------------
     * The real drive reports "DVDR   PX-716A" with THREE spaces; the table
     * spells it with one. Without whitespace collapse this lookup misses and
     * every PX-716A rip goes uncorrected. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("PLEXTOR", "DVDR   PX-716A", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 30);

    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("  PLEXTOR  ", " DVDR  PX-716A ", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 30);

    /* --- matching is case-INSENSITIVE, deliberately -----------------------
     * Vendors are not consistent about capitalisation — this corpus carries
     * "AOpen" and "AOPEN", "hp" and "HP" for one company — so a case-sensitive
     * compare splits one drive into two keys and answers only for whichever
     * casing the firmware used. The table is emitted upper-cased and the lookup
     * folds, so every casing a drive can report reaches the same row.
     *
     * This assertion was INVERTED on 2026-08-16: it previously required
     * NOTFOUND. Verified lossless before changing it — of 5888 rows, zero pairs
     * differed only by case, so nothing that was distinct became ambiguous. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("plextor", "dvdr px-716a", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 30);

    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("PlExToR", "dVdR   Px-716a", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 30);

    /* Folding must not make DIFFERENT drives collide. These two really are
     * distinct products — a 12x and a 16x AOpen reader with different offsets —
     * and the speed token is the only thing separating them. If a future
     * "cleaning" step ever strips it, these two assertions fail together. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("AOPEN", "12X DVD-ROM-AMH", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 691);
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("aopen", "16XDVD-ROM-AMH", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 102);

    /* --- the key that used to disagree, and why it no longer does --------
     * TEAC DW-224E-CN was the table's one unresolvable conflict: REDUMP +120
     * against AccurateRip +102, nothing to adjudicate between them. It was
     * never two opinions. REDUMP's offset table is AccurateRip's list imported
     * once in 2022 and frozen, and that import carried this row as
     *
     *     TEAC - DW-224E-CN   +120   2 submissions   50% agree
     *
     * — AccurateRip's own value at the time, resting on two submissions that
     * disagreed with each other. AccurateRip has since settled it at +102 on
     * seven agreeing submissions. The generator drops a REDUMP value its source
     * has superseded, so one number comes back now.
     *
     * `sources` is the assertion that matters: AR ALONE. If the REDUMP claim
     * were still being merged this would read as both, and the +120 would be
     * back in values[]. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("TEAC", "DW-224E-CN", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 102);
    assert(info.n_values == 1);
    assert(info.sources == ACCUDISC_OFFSET_SRC_AR);
    assert((info.flags & ACCUDISC_OFFSET_F_CONFLICT) == 0);

    /* --- a retracted row is ABSENT, not merely unfavoured -----------------
     * PHILIPS CDRW5232P1 (+732, 2 submissions, 50% agree in the 2022 import)
     * is gone from AccurateRip's live list entirely. A withdrawn measurement
     * must not reach a caller as data.
     *
     * THIS IS THE GUARD ON THE GENERATOR'S INPUTS. Regenerate the table without
     * --redump-provenance and the rule cannot run; the row comes back and this
     * assertion fails, which is the whole reason that argument is required
     * rather than optional. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("PHILIPS", "CDRW5232P1", &info)
           == ACCUDISC_ERR_NOTFOUND);
    assert(info.read_offset == ACCUDISC_OFFSET_NONE);

    /* --- AccurateRip's own count decides, and REDUMP no longer contradicts --
     * PIONEER BD-RW BDR-206 is +667 on 1065 AccurateRip submissions; the same
     * page also listed +0 on 4, and REDUMP's frozen copy held the +0. Reading
     * the last row printed, or treating the two as comparable evidence, inverts
     * a 1065-to-4 verdict while looking perfectly well-formed. Both sources
     * now hold +667 — REDUMP because its stale value was withdrawn, which is
     * why this key reports BOTH rather than AR alone. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("PIONEER", "BD-RW BDR-206", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 667);
    assert(info.sources
           == (ACCUDISC_OFFSET_SRC_REDUMP | ACCUDISC_OFFSET_SRC_AR));
    assert(info.ar_submissions == 1065);

    /* --- absence is explicit, never a default ---------------------------- */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("NOSUCHVENDOR", "NOSUCH 9000", &info)
           == ACCUDISC_ERR_NOTFOUND);
    assert(info.read_offset == ACCUDISC_OFFSET_NONE);

    /* --- argument and ABI guards ----------------------------------------- */
    assert(accudisc_offset_for_inquiry(NULL, "X", &info) == ACCUDISC_ERR_INVAL);
    assert(accudisc_offset_for_inquiry("X", NULL, &info) == ACCUDISC_ERR_INVAL);
    assert(accudisc_offset_for_inquiry("X", "Y", NULL) == ACCUDISC_ERR_INVAL);

    /* A zeroed struct declares no size, so the call cannot know which fields
     * the caller allocated and must refuse rather than write past them. */
    memset(&info, 0, sizeof(info));
    assert(accudisc_offset_for_inquiry("PLEXTOR", "DVDR PX-716A", &info)
           == ACCUDISC_ERR_ABI);
    /* A struct claiming to be LARGER than this build's is a caller from the
     * future; refuse rather than zero memory that may not be ours. */
    info.size = sizeof(info) + 8;
    assert(accudisc_offset_for_inquiry("PLEXTOR", "DVDR PX-716A", &info)
           == ACCUDISC_ERR_ABI);

    /* The sentinel must not be a value any drive could legitimately have. */
    assert(ACCUDISC_OFFSET_NONE != 0);
    assert(ACCUDISC_OFFSET_NONE < -100000);

    return 0;
}
