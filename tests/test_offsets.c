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

    /* --- matching is case-sensitive, deliberately ------------------------
     * The comparison is against bytes a drive reported, not a curated name.
     * The generator emits every spelling it saw rather than folding here. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("plextor", "dvdr px-716a", &info)
           == ACCUDISC_ERR_NOTFOUND);

    /* --- a key whose sources disagree ------------------------------------
     * TEAC DW-224E-CN: REDUMP says +120, AccurateRip says +102, and nothing
     * adjudicates. The contract is that no single number comes back. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("TEAC", "DW-224E-CN", &info)
           == ACCUDISC_ERR_AMBIGUOUS);
    assert(info.flags & ACCUDISC_OFFSET_F_CONFLICT);
    assert(info.n_values == 2);
    assert(info.values[0] != info.values[1]);
    /* THE point of the whole struct: a caller that ignored the return code must
     * not find something it can apply. Zero would be applicable and wrong. */
    assert(info.read_offset == ACCUDISC_OFFSET_NONE);
    assert(info.read_offset != 0);
    /* Each candidate carries its own provenance, and between them they cover
     * both sources — otherwise the caller cannot tell who claims what. */
    assert((info.value_sources[0] | info.value_sources[1])
           == (ACCUDISC_OFFSET_SRC_REDUMP | ACCUDISC_OFFSET_SRC_AR));
    assert(info.value_sources[0] != info.value_sources[1]);

    /* --- an adjudicated key reports that it was adjudicated ---------------
     * PIONEER BD-RW BDR-206 is +667 on 1065 AccurateRip submissions and +0 on
     * 4. The resolution is visible rather than silent, which is the difference
     * between adjudication and a first-match rule. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("PIONEER", "BD-RW BDR-206", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 667);
    assert(info.flags & ACCUDISC_OFFSET_F_ADJUDICATED);

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
