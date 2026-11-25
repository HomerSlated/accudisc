/* accudisc_read_policy, revision 1 (0.58.0; AccuDisc correspondence
 * 2026-11-25k/l/y, cdda2img 274/278).
 *
 * A pure function, so every case asserts the EXACT request and reason bits,
 * derived by hand from the table in accudisc.h. Plus: every refusal leaves out
 * and info byte-for-byte as they were; the caller's own fields are never
 * written; every policy-owned field is overwritten (pre-filled with garbage).
 * The same outputs are run through the read engine on a fake drive in
 * test_effort.c. */

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "accudisc/accudisc.h"

#define R(x) ACCUDISC_POLICY_R_##x

static const uint16_t five[] = { 24, 8, 40, 32, 16 }; /* deliberately unsorted */

static accudisc_read_intent intent(uint8_t purpose, uint16_t speed)
{
    accudisc_read_intent in = ACCUDISC_READ_INTENT_INIT;

    in.purpose = purpose;
    in.c2_usable = 1;
    in.n_admitted = 5;
    memcpy(in.admitted_x, five, sizeof(five));
    in.speed_x = speed;
    in.lba = 10000;
    in.count = 2000;
    in.readable_lo = 0;
    in.readable_hi = 200000;
    in.budget_cmds = 777;
    in.budget_speed_changes = 9;
    return in;
}

/* out pre-filled with GARBAGE everywhere and sentinels in the caller's own
 * fields: after a successful call every policy-owned field must have been
 * overwritten, and the caller's must be untouched. */
static uint8_t sm, qm, om;
static const volatile int cflag;

static void garbage(accudisc_read_req *out)
{
    memset(out, 0xA5, sizeof(*out));
    out->size = sizeof(*out);
    out->status_map = &sm;
    out->subq_map = &qm;
    out->origin_map = &om;
    out->cancel = &cflag;
    out->buffer_bytes = 0x12345678u;
}

static void callers_fields_untouched(const accudisc_read_req *out)
{
    assert(out->status_map == &sm && out->subq_map == &qm);
    assert(out->origin_map == &om && out->cancel == &cflag);
    assert(out->buffer_bytes == 0x12345678u);
}

static int call(const accudisc_read_intent *in, accudisc_read_req *out,
                accudisc_read_policy_info *info)
{
    garbage(out);
    memset(info, 0x5A, sizeof(*info));
    info->size = sizeof(*info);
    int rc = accudisc_read_policy(in, out, info);
    if (rc == ACCUDISC_OK) {
        callers_fields_untouched(out);
        assert(info->revision == 1 && ACCUDISC_POLICY_REVISION == 1);
        assert(info->size == sizeof(*info));
        /* fixed in every row of revision 1 */
        assert(out->lba == in->lba && out->count == in->count);
        assert(out->any_type == 0 && out->retries == 0);
        assert(out->chunk_sectors == 0 && out->overlap_sectors == 0);
        assert(out->c2_retries == 0 && out->c2_witness == 0);
        assert(out->shift_search == 0);
        assert(out->speed_x == in->speed_x);
        assert(out->readable_lo == in->readable_lo &&
               out->readable_hi == in->readable_hi);
        assert(out->budget_cmds == in->budget_cmds &&
               out->budget_speed_changes == in->budget_speed_changes);
        if (out->ladder_len)
            assert(out->speed_ladder == info->ladder);
        else
            assert(out->speed_ladder == NULL);
        for (unsigned i = out->ladder_len; i < 16; i++)
            assert(info->ladder[i] == 0);
    }
    return rc;
}

static void test_capture(void)
{
    accudisc_read_req out;
    accudisc_read_policy_info info;
    accudisc_read_intent in = intent(ACCUDISC_PURPOSE_CAPTURE, 40);

    assert(call(&in, &out, &info) == ACCUDISC_OK);
    assert(out.c2 == ACCUDISC_C2_PTRS && out.sub == ACCUDISC_SUB_RAW);
    assert(out.cache_defeat == 0 && out.verify_passes == 0);
    assert(out.ladder_len == 0 && out.rescue_by_rung == 0);
    assert(info.reasons == 0);

    in.c2_usable = 0; /* no C2, drive-managed speed */
    in.speed_x = 0;
    assert(call(&in, &out, &info) == ACCUDISC_OK);
    assert(out.c2 == ACCUDISC_C2_NONE && out.sub == ACCUDISC_SUB_RAW);
    assert(info.reasons == R(NO_C2));
    printf("ok - CAPTURE: PTRS/RAW, nothing else; no C2 -> NONE, R_NO_C2\n");
}

/* The head far read: 10000 - 5000 is in [0, 200000), so the defeat is on. A
 * range covering the whole readable extent has no target: off, with the
 * reason (cdda2img 274). Unknown range: the engine's unbounded rule (forward
 * from the end of [0, 195656)), on, with R_READABLE_UNKNOWN. A target only
 * NEAR (in range, closer than 5000) still counts as a target. */
static void test_gated(void)
{
    accudisc_read_req out;
    accudisc_read_policy_info info;
    accudisc_read_intent in = intent(ACCUDISC_PURPOSE_GATED_REREAD, 32);

    assert(call(&in, &out, &info) == ACCUDISC_OK);
    assert(out.c2 == ACCUDISC_C2_PTRS && out.sub == ACCUDISC_SUB_NONE);
    assert(out.cache_defeat == 1 && out.verify_passes == 0);
    assert(out.ladder_len == 0 && out.rescue_by_rung == 0);
    assert(info.reasons == R(ENGINE_RECOVERY_PENDING_AB));

    in.lba = 0; /* whole-disc re-read of session 1 */
    in.count = 195656;
    in.readable_hi = 195656;
    assert(call(&in, &out, &info) == ACCUDISC_OK);
    assert(out.cache_defeat == 0);
    assert(info.reasons ==
           (R(ENGINE_RECOVERY_PENDING_AB) | R(DEFEAT_NO_TARGET)));

    in.readable_hi = 0; /* unknown */
    assert(call(&in, &out, &info) == ACCUDISC_OK);
    assert(out.cache_defeat == 1);
    assert(info.reasons ==
           (R(ENGINE_RECOVERY_PENDING_AB) | R(READABLE_UNKNOWN)));

    in.lba = 2000; /* [2000, 2046) in [0, 6000): only 0 is a target, NEAR */
    in.count = 46;
    in.readable_hi = 6000;
    assert(call(&in, &out, &info) == ACCUDISC_OK);
    assert(out.cache_defeat == 1);
    assert(info.reasons == R(ENGINE_RECOVERY_PENDING_AB));

    in.c2_usable = 0;
    assert(call(&in, &out, &info) == ACCUDISC_OK);
    assert(out.c2 == ACCUDISC_C2_NONE);
    assert(info.reasons == (R(ENGINE_RECOVERY_PENDING_AB) | R(NO_C2)));
    printf("ok - GATED_REREAD: defeat on; off with R_DEFEAT_NO_TARGET over the "
           "whole extent; unknown range flagged; NEAR counts\n");
}

static void test_span_rescue(void)
{
    accudisc_read_req out;
    accudisc_read_policy_info info;
    const uint64_t base = R(C2_RESCUE_KEPT_OFF) | R(SHIFT_SEARCH_PENDING_AB);

    accudisc_read_intent in = intent(ACCUDISC_PURPOSE_SPAN_RESCUE, 24);
    assert(call(&in, &out, &info) == ACCUDISC_OK);
    assert(out.c2 == ACCUDISC_C2_PTRS && out.sub == ACCUDISC_SUB_RAW);
    assert(out.cache_defeat == 1 && out.verify_passes == 2);
    assert(out.rescue_by_rung == 1 && out.shift_search == 0);
    assert(out.ladder_len == 2 && info.ladder[0] == 16 && info.ladder[1] == 8);
    assert(info.reasons == base);

    in.speed_x = 40; /* the top: every lower rung, descending */
    assert(call(&in, &out, &info) == ACCUDISC_OK);
    assert(out.ladder_len == 4 && info.ladder[0] == 32 && info.ladder[1] == 24 &&
           info.ladder[2] == 16 && info.ladder[3] == 8);
    assert(info.reasons == base);

    in.speed_x = 8; /* the lowest: the next one up */
    assert(call(&in, &out, &info) == ACCUDISC_OK);
    assert(out.ladder_len == 1 && info.ladder[0] == 16);
    assert(info.reasons == (base | R(LADDER_NEXT_UP)));

    in.n_admitted = 1; /* the only rung: no ladder */
    in.admitted_x[0] = 8;
    assert(call(&in, &out, &info) == ACCUDISC_OK);
    assert(out.ladder_len == 0);
    assert(info.reasons == (base | R(NO_LADDER)));
    printf("ok - SPAN_RESCUE: verify 2, rungs below descending, next up at "
           "the bottom, none alone; rescue_by_rung on, shift_search off\n");
}

/* drive_slips == 2 changes NOTHING but the reason bit. */
static void test_slips(void)
{
    static const uint8_t purposes[] = { ACCUDISC_PURPOSE_CAPTURE,
                                        ACCUDISC_PURPOSE_GATED_REREAD,
                                        ACCUDISC_PURPOSE_SPAN_RESCUE };
    for (unsigned p = 0; p < 3; p++) {
        accudisc_read_req a, b;
        accudisc_read_policy_info ia, ib;
        accudisc_read_intent in = intent(purposes[p], 24);

        assert(call(&in, &a, &ia) == ACCUDISC_OK);
        in.drive_slips = 2;
        assert(call(&in, &b, &ib) == ACCUDISC_OK);
        assert(ib.reasons == (ia.reasons | R(SLIPS_KNOWN_RELATIVE_ONLY)));
        assert(!(ia.reasons & R(SLIPS_KNOWN_RELATIVE_ONLY)));
        b.speed_ladder = a.speed_ladder = NULL; /* they point into ia / ib */
        assert(memcmp(&a, &b, sizeof(a)) == 0);
        assert(memcmp(ia.ladder, ib.ladder, sizeof(ia.ladder)) == 0);
        in.drive_slips = 1;
        assert(call(&in, &b, &ib) == ACCUDISC_OK);
        assert(ib.reasons == ia.reasons);
    }
    printf("ok - drive_slips 2 sets only R_SLIPS_KNOWN_RELATIVE_ONLY\n");
}

/* A refusal writes NOTHING: out and info stay byte-identical. */
static void refuse(const accudisc_read_intent *in, int want, uint32_t out_size,
                   uint32_t info_size)
{
    accudisc_read_req out, out0;
    accudisc_read_policy_info info, info0;

    garbage(&out);
    out.size = out_size;
    memset(&info, 0x5A, sizeof(info));
    info.size = info_size;
    out0 = out;
    info0 = info;
    assert(accudisc_read_policy(in, &out, &info) == want);
    assert(memcmp(&out, &out0, sizeof(out)) == 0);
    assert(memcmp(&info, &info0, sizeof(info)) == 0);
}

static void test_refusals(void)
{
    const uint32_t O = sizeof(accudisc_read_req);
    const uint32_t I = sizeof(accudisc_read_policy_info);
    accudisc_read_intent in;

#define BAD(stmt, rc)                                                          \
    do {                                                                       \
        in = intent(ACCUDISC_PURPOSE_SPAN_RESCUE, 24);                         \
        stmt;                                                                  \
        refuse(&in, rc, O, I);                                                 \
    } while (0)

    BAD(in.purpose = 0, ACCUDISC_ERR_INVAL);
    BAD(in.purpose = 5, ACCUDISC_ERR_INVAL);
    BAD(in.purpose = ACCUDISC_PURPOSE_REFERENCE_VERIFY, ACCUDISC_ERR_UNSUPPORTED);
    BAD(in.count = 0, ACCUDISC_ERR_INVAL);
    BAD((in.readable_lo = 5, in.readable_hi = 5), ACCUDISC_ERR_INVAL);
    BAD((in.readable_lo = 6, in.readable_hi = 5), ACCUDISC_ERR_INVAL);
    BAD(in.n_admitted = 17, ACCUDISC_ERR_INVAL);
    BAD(in.admitted_x[2] = 0, ACCUDISC_ERR_INVAL);
    BAD(in.admitted_x[4] = 24, ACCUDISC_ERR_INVAL);        /* a duplicate */
    BAD(in.admitted_x[0] = in.admitted_x[1], ACCUDISC_ERR_INVAL);
    BAD(in.speed_x = 20, ACCUDISC_ERR_INVAL);              /* not admitted */
    BAD(in.n_admitted = 0, ACCUDISC_ERR_INVAL);            /* 24 not admitted */
    BAD(in.speed_x = 0, ACCUDISC_ERR_INVAL);               /* rescue needs it */
    BAD(in.c2_usable = 0, ACCUDISC_ERR_INVAL);             /* rescue needs C2 */
    BAD(in.size = 0, ACCUDISC_ERR_ABI);
#undef BAD

    /* 16 rungs is the limit, exactly. 17 is refused on the COUNT, not by
     * luck: with 16 distinct rungs 1..16 and speed_x 17, a 17th entry read
     * past the array would be speed_x itself, distinct and admitted. */
    {
        accudisc_read_req o;
        accudisc_read_policy_info pi;

        in = intent(ACCUDISC_PURPOSE_SPAN_RESCUE, 16);
        in.n_admitted = 16;
        for (unsigned i = 0; i < 16; i++)
            in.admitted_x[i] = (uint16_t)(16 - i);
        assert(call(&in, &o, &pi) == ACCUDISC_OK);
        assert(o.ladder_len == 15 && pi.ladder[0] == 15 && pi.ladder[14] == 1);
        in.n_admitted = 17;
        in.speed_x = 17;
        refuse(&in, ACCUDISC_ERR_INVAL, O, I);
    }

    /* speed_x 0 is drive-managed: fine for CAPTURE and GATED_REREAD. */
    accudisc_read_req out;
    accudisc_read_policy_info info;
    in = intent(ACCUDISC_PURPOSE_GATED_REREAD, 0);
    assert(call(&in, &out, &info) == ACCUDISC_OK);
    in.n_admitted = 0;
    assert(call(&in, &out, &info) == ACCUDISC_OK);

    /* Sizes: out must reach the last field the policy writes (112 in 0.58)
     * and not exceed ours; info must be exactly ours. */
    in = intent(ACCUDISC_PURPOSE_CAPTURE, 24);
    refuse(&in, ACCUDISC_ERR_ABI, 0, I);
    refuse(&in, ACCUDISC_ERR_ABI, 104, I);
    refuse(&in, ACCUDISC_ERR_ABI, O - 1, I);
    refuse(&in, ACCUDISC_ERR_ABI, O + 8, I);
    refuse(&in, ACCUDISC_ERR_ABI, O, 0);
    refuse(&in, ACCUDISC_ERR_ABI, O, I - 8);
    refuse(&in, ACCUDISC_ERR_ABI, O, I + 8);
    assert(accudisc_read_policy(NULL, &out, &info) == ACCUDISC_ERR_INVAL);
    assert(accudisc_read_policy(&in, NULL, &info) == ACCUDISC_ERR_INVAL);
    assert(accudisc_read_policy(&in, &out, NULL) == ACCUDISC_ERR_INVAL);
    printf("ok - every refusal, and none writes out or info\n");
}

/* The IN rule: an intent declaring a shorter size zero-extends. Cut it before
 * budget_cmds and the budget reads as 0, whatever lies past the cut. */
static void test_short_intent(void)
{
    accudisc_read_req out;
    accudisc_read_policy_info info;
    accudisc_read_intent in = intent(ACCUDISC_PURPOSE_CAPTURE, 24);

    in.size = offsetof(accudisc_read_intent, budget_cmds);
    garbage(&out);
    info.size = sizeof(info);
    assert(accudisc_read_policy(&in, &out, &info) == ACCUDISC_OK);
    assert(out.budget_cmds == 0 && out.budget_speed_changes == 9);
    assert(sizeof(accudisc_read_intent) == 64);
    assert(sizeof(accudisc_read_policy_info) == 48);
    printf("ok - a shorter intent zero-extends; intent 64, info 48 bytes\n");
}

int main(void)
{
    test_capture();
    test_gated();
    test_span_rescue();
    test_slips();
    test_refusals();
    test_short_intent();
    return 0;
}
