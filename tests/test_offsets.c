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

    /* --- AccurateRip duplicates that AGREE are pooled, not selected -------
     * AccurateRip lists 4878 rows under 4775 keys; 91 of the duplicated keys
     * carry the SAME offset in every row. Keeping only the largest discards
     * real measurements — "DVD RW" is listed at 298 and 267, both +6, and used
     * to ship as 298 when 565 people had measured it. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("", "DVD RW", &info) == ACCUDISC_OK);
    assert(info.read_offset == 6);
    assert(info.ar_submissions == 565);

    /* Agreement across a pool is the submission-WEIGHTED mean, which is the only
     * average that keeps "percent of submissions agreeing" meaning what it says.
     * TSSTCORP CDDVDW SE-218GN is the corpus's one pooled key whose rows differ
     * on agreement: 193 at 100% and 4 at 75%.
     *
     *     weighted   (193*100 + 4*75) / 197 = 99
     *     flat mean  (100 + 75) / 2         = 88   <- wrong, and plausible
     *
     * 88 is what an unweighted average gives, and nothing downstream could tell
     * it from the truth — which is why this assertion names the number. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("TSSTCORP", "CDDVDW SE-218GN", &info)
           == ACCUDISC_OK);
    assert(info.ar_submissions == 197);
    assert(info.ar_agree_pct == 99);

    /* --- a separator is a spelling, not a different drive -----------------
     * AccurateRip writes one drive both ways. HL-DT-ST DVDRAM_GHA2N was in
     * REDUMP's 2022 import and is absent from the live list under THAT spelling,
     * so the retraction rule dropped it — while LG's DVDRAM GHA2N, the same
     * drive with a space, was live on 71 submissions the whole time. Folding
     * underscore to space in the build-time key joins them: measured over the
     * union corpus, it merges 26 groups of keys, all 26 agreeing on the offset
     * and none disagreeing.
     *
     * THE FOLD IS BUILD-TIME ONLY. adsc_inquiry_normalize folds case and
     * whitespace, never underscores, so each spelling has to be its own row or
     * the drive reporting it gets ERR_NOTFOUND. These three assertions are that
     * claim: same drive, same answer, three strings. There is deliberately no
     * "LG ELECTRONICS DVDRAM_GHA2N" — no source ever reported that spelling, and
     * the generator emits what was seen rather than every combination it could
     * construct. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("HL-DT-ST", "DVDRAM_GHA2N", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 667);
    assert(info.ar_submissions == 71);
    assert(info.sources
           == (ACCUDISC_OFFSET_SRC_REDUMP | ACCUDISC_OFFSET_SRC_AR));

    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("HL-DT-ST", "DVDRAM GHA2N", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 667);
    assert(info.ar_submissions == 71);

    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("LG Electronics", "DVDRAM GHA2N", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 667);
    assert(info.ar_submissions == 71);

    /* Pooling used to return ONE row per key and drop the other spellings with
     * it. That was not a consequence of the underscore fold — it cost
     * TSSTcorp/CDDVDW before the fold was ever proposed, and that drive answered
     * ERR_NOTFOUND while the identical measurement shipped under the vendorless
     * spelling. The generator now asserts, against a fresh read of its input,
     * that every name it was given reaches a row. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("TSSTcorp", "CDDVDW", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 6);
    assert(info.ar_submissions == 43);

    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("", "TSSTCORP CDDVDW", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 6);
    assert(info.ar_submissions == 43);

    /* --- a rebadge is rescued, and rescued is not corroborated -------------
     * A rebadged drive reports the REBADGE string over INQUIRY, so dropping its
     * row strands its owner while the same measurement ships under the OEM name.
     * Philips DVD-ROM PCDV632 is a Toshiba SD-M1212 OEM drive (rpc1.org), the
     * two agree at +116, and the generator keeps the Philips row on that
     * REVIEWED mapping alone — never on "some live row shares this offset",
     * which is worthless here: +116 has 43 live rows.
     *
     * IT SHIPS AS REDUMP-ONLY WITH NO AR FIGURES, and that is the honest part.
     * Attaching SD-M1212's 38 submissions would claim 38 people measured a drive
     * under a name AccurateRip does not list. The corroboration is real but it
     * is about the OEM twin, so it lives in the RESCUED comment block of
     * offsets_db.inc rather than in this row's numbers. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("Philips", "DVD-ROM PCDV632", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 116);
    assert(info.sources == ACCUDISC_OFFSET_SRC_REDUMP);
    assert(info.ar_submissions == 0);

    /* The twin it rests on, and the agreement that licensed the rescue. If this
     * ever moves, the generator refuses to emit rather than leaving the rescued
     * row citing a corroboration that is no longer there. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("TOSHIBA", "DVD-ROM SD-M1212", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 116);
    assert(info.ar_submissions == 38);

    /* Rescue is RETRACTED-only and one line per human decision, so the other
     * withdrawn rows stay withdrawn: no mapping was written for CDRW5232P1, and
     * a SUPERSEDED row would never be eligible anyway — there AccurateRip
     * corrected the value rather than removing the name. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("PHILIPS", "PCRW404", &info)
           == ACCUDISC_ERR_NOTFOUND);

    /* --- the key is the PRODUCT, against the SHIPPED table -----------------
     * A drive whose vendor string is not the one a submitter sent still
     * resolves. This is the whole point of product-only keying: firmware
     * reports that field inconsistently, and "PLEXTOR" is simply the spelling
     * that reached AccurateRip. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("SATA", "DVDR PX-716A", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 30);

    /* CD-ROM is the worked example: FIVE rows, FOUR offsets, and it is the
     * corpus's worst case — exactly ACCUDISC_OFFSET_MAX_VALUES, so values[]
     * fills completely and F_TRUNCATED must NOT be set. A vendor nobody
     * submitted narrows nothing and the caller is handed all four. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("NOSUCHVENDOR", "CD-ROM", &info)
           == ACCUDISC_ERR_AMBIGUOUS);
    assert(info.read_offset == ACCUDISC_OFFSET_NONE);
    assert(info.n_values == 4);
    assert(!(info.flags & ACCUDISC_OFFSET_F_TRUNCATED));

    /* ...and the vendor resolves it, which is true of all 13 ambiguous
     * products in the shipped table. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("ATAPI", "CD-ROM", &info) == ACCUDISC_OK);
    assert(info.read_offset == 680);

    /* Several rows agreeing is NOT ambiguity. This drive has three spellings in
     * the table — two vendors, two product spellings — and one offset. Counting
     * matching ROWS rather than distinct offsets would report ERR_AMBIGUOUS
     * here, and for 1229 other products. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("", "DVDRAM GHA2N", &info) == ACCUDISC_OK);
    assert(info.read_offset == 667);
    assert(info.n_values == 1);
    assert(info.ar_submissions == 71);

    /* The one answer in the whole table that 0.15.0 changed: ("DVDROM", "")
     * at +564, which keyed on the product alone would have answered for every
     * drive reporting no product string.
     *
     * READ THIS BEFORE TRUSTING THE ASSERTION. In 0.17.0 that row was RETRACTED
     * — it was in AccurateRip's 2022 list, is absent from the live one, and had
     * escaped the retraction rule only because "DVDROM -" (a trailing separator,
     * empty product) mis-parsed and joined nothing. Fixing the parse dropped it,
     * and it was the LAST empty-product row: the shipped table now has zero.
     *
     * So this call can no longer distinguish the empty-product guard FIRING from
     * the key simply being absent — two causes, one return code. It is kept as a
     * regression pin on the retraction (a corpus refresh that resurrects the row
     * must not resurrect the answer), NOT as a test of the guard. The guard is
     * tested where it can fail: tests/test_offsets_ambiguous.c asserts EMPTYP
     * against a fixture that really does hold an empty-product row. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("DVDROM", "", &info)
           == ACCUDISC_ERR_NOTFOUND);

    /* --- the six generic product names, against the SHIPPED table ---------
     * "OPTICAL DRIVE" names a category, not a model. It rests on 85 real
     * submissions under BUFFALO and still answers for BUFFALO — the block is on
     * the product-only PATH, not the row, because dropping the row would throw
     * away a measurement to fix a matching rule. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("BUFFALO", "OPTICAL DRIVE", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 6);
    assert(info.ar_submissions == 85);
    assert(info.flags & ACCUDISC_OFFSET_F_GENERIC);

    /* Any other drive reporting the same category string gets nothing, where
     * 0.15.0 handed it BUFFALO's +6 with the confidence of an exact match. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("SONY", "OPTICAL DRIVE", &info)
           == ACCUDISC_ERR_NOTFOUND);
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("", "DVD+RW", &info)
           == ACCUDISC_ERR_NOTFOUND);
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("ATAPI", "DVD+RW", &info) == ACCUDISC_OK);
    assert(info.read_offset == 1292);

    /* --- and the two that are FRAGMENTS rather than categories -------------
     * The INQUIRY vendor field is eight bytes; a longer name continues into the
     * product field. "DVDROM 8X" and "DVDROM 10X" are cut there, leaving "X" and
     * "0X" as the product. A ONE-CHARACTER product was answering +564 for any
     * vendor at all — the same hazard as a category word, from a different
     * cause, so it gets the same remedy. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("NOSUCHVENDOR3", "X", &info)
           == ACCUDISC_ERR_NOTFOUND);
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("", "0X", &info)
           == ACCUDISC_ERR_NOTFOUND);

    /* The rows are NOT dropped: +564 is what that generation of drive measures
     * (14 other rows hold it, HITACHI DVD-ROM GD-2500 on 48 submissions among
     * them), so each still answers for the vendor half it was measured under —
     * which is the whole of the drive's reported identity. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("DVDROM 8", "X", &info) == ACCUDISC_OK);
    assert(info.read_offset == 564);
    assert(info.ar_submissions == 2);
    assert(info.flags & ACCUDISC_OFFSET_F_GENERIC);
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("DVDROM 1", "0X", &info) == ACCUDISC_OK);
    assert(info.read_offset == 564);
    assert(info.ar_submissions == 8);

    /* NOT extended to every fragment either. "-952E-AKV" is just as much a
     * spill ("E-IDE CD" + "-952E-AKV" = E-IDE CD-952E-AKV) and stays reachable
     * on the product alone, because it is DISTINCTIVE: nothing but the drive it
     * was cut from will report a model number like that, so answering for any
     * vendor answers for the right drive. "X" is the opposite. The line is
     * "could another drive plausibly report this string?", not "is this a
     * fragment?" — a stated choice, pinned here so it cannot drift into an
     * oversight. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("ARBITRARY", "-952E-AKV", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 691);
    assert(!(info.flags & ACCUDISC_OFFSET_F_GENERIC));

    /* NOT extended to the generic names that COLLIDE. "CD-ROM" is at least as
     * generic, and is deliberately still reachable: it refuses to pick rather
     * than picking wrongly, which is a different and safer failure. Blocking it
     * would remove information without removing a hazard. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("NOSUCHVENDOR2", "CD-ROM", &info)
           == ACCUDISC_ERR_AMBIGUOUS);

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
