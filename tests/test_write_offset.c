/* Write-offset measurement: the signal and the locator.
 *
 * THE TEST THAT MATTERS IS THE ONE WITH A NON-ZERO READ OFFSET. Measuring on
 * CDEmu gives write_offset 0 and read_offset 0, and with both zero the
 * correction term drops out of the arithmetic entirely — a sign error in it is
 * invisible, and a green run would prove the plumbing while proving nothing
 * about the sum. So the discriminating case is built here, with no drive:
 *
 *     place the pulses at  expected + R + W,  pass read_offset = R,
 *     require the result to be exactly W
 *
 * R and W are chosen with OPPOSITE SIGNS and |R| != |W| so that no compensating
 * error survives: adding R where it should be subtracted yields W + 2R, which
 * is neither W nor -W nor 0.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accudisc/accudisc.h"

#define N ACCUDISC_WOFF_SAMPLES

/* raw[i] = signal[i - shift]: the whole disc laid down `shift` samples late. */
static void shift_into(int16_t *dst, const int16_t *src, int32_t shift)
{
    int64_t i;

    memset(dst, 0, (size_t)N * 2 * sizeof(int16_t));
    for (i = 0; i < (int64_t)N; i++) {
        int64_t s = i - shift;
        if (s < 0 || s >= (int64_t)N)
            continue;
        dst[i * 2]     = src[s * 2];
        dst[i * 2 + 1] = src[s * 2 + 1];
    }
}

int main(void)
{
    accudisc_write_offset_info info = ACCUDISC_WRITE_OFFSET_INFO_INIT;
    int16_t *sig = malloc((size_t)N * 2 * sizeof(int16_t));
    int16_t *disc = malloc((size_t)N * 2 * sizeof(int16_t));
    int32_t R, W;

    assert(sig && disc);

    /* --- the signal ------------------------------------------------------ */
    assert(accudisc_write_offset_signal(sig, N) == ACCUDISC_OK);
    /* Exactly, not "close": deterministic output gets exact equality. This is
     * what pins the forced full-scale leading edge — with a random first sample
     * this would occasionally be PULSE_A + 1. */
    info = (accudisc_write_offset_info)ACCUDISC_WRITE_OFFSET_INFO_INIT;
    assert(accudisc_write_offset_locate(sig, N, 0, &info) == ACCUDISC_OK);
    assert(info.write_offset == 0);
    assert(info.found_a == (int32_t)ACCUDISC_WOFF_PULSE_A);
    assert(info.found_b == (int32_t)ACCUDISC_WOFF_PULSE_B);
    assert(info.offset_a == 0 && info.offset_b == 0);
    assert(info.flags == 0);

    /* Deterministic across calls — a disc measured months later must compare. */
    {
        int16_t *again = malloc((size_t)N * 2 * sizeof(int16_t));
        assert(again);
        assert(accudisc_write_offset_signal(again, N) == ACCUDISC_OK);
        assert(memcmp(sig, again, (size_t)N * 2 * sizeof(int16_t)) == 0);
        free(again);
    }

    /* A short buffer would put pulse B off the end and halve the measurement. */
    assert(accudisc_write_offset_signal(sig, N - 1) == ACCUDISC_ERR_INVAL);
    assert(accudisc_write_offset_signal(NULL, N) == ACCUDISC_ERR_INVAL);
    /* ...and _signal must not have been left half-written by that refusal. */
    assert(accudisc_write_offset_signal(sig, N) == ACCUDISC_OK);

    /* --- THE DISCRIMINATOR ------------------------------------------------
     * R = +667 (a real corpus value), W = -30 (opposite sign, different
     * magnitude). Add-instead-of-subtract gives W + 2R = +1304; dropping the
     * term gives W + R = +637; negating W gives +30. None is -30. */
    R = 667;
    W = -30;
    shift_into(disc, sig, R + W);
    info = (accudisc_write_offset_info)ACCUDISC_WRITE_OFFSET_INFO_INIT;
    assert(accudisc_write_offset_locate(disc, N, R, &info) == ACCUDISC_OK);
    assert(info.write_offset == W);
    assert(info.offset_a == W && info.offset_b == W);
    assert(info.found_a == (int32_t)ACCUDISC_WOFF_PULSE_A + W);

    /* The mirror: negative read offset, positive write offset. */
    R = -102;
    W = 6;
    shift_into(disc, sig, R + W);
    info = (accudisc_write_offset_info)ACCUDISC_WRITE_OFFSET_INFO_INIT;
    assert(accudisc_write_offset_locate(disc, N, R, &info) == ACCUDISC_OK);
    assert(info.write_offset == W);

    /* Zero write offset with a NON-zero read offset — the case CDEmu cannot
     * produce, and the one a real drive will. */
    R = 30;
    W = 0;
    shift_into(disc, sig, R + W);
    info = (accudisc_write_offset_info)ACCUDISC_WRITE_OFFSET_INFO_INIT;
    assert(accudisc_write_offset_locate(disc, N, R, &info) == ACCUDISC_OK);
    assert(info.write_offset == 0);

    /* --- absence is explicit --------------------------------------------- */
    memset(disc, 0, (size_t)N * 2 * sizeof(int16_t));
    info = (accudisc_write_offset_info)ACCUDISC_WRITE_OFFSET_INFO_INIT;
    assert(accudisc_write_offset_locate(disc, N, 0, &info)
           == ACCUDISC_ERR_NOTFOUND);
    assert(info.write_offset == ACCUDISC_OFFSET_NONE);
    assert(info.found_a == ACCUDISC_OFFSET_NONE);
    assert(info.found_b == ACCUDISC_OFFSET_NONE);

    /* An isolated loud sample is a click, not a burst, and must not be taken
     * for one — it sits EARLIER in the window, so accepting it would move the
     * answer by thousands of samples.
     *
     * THE BUFFER MUST OTHERWISE BE INTACT. The first version of this check put
     * the click into a SILENT buffer and asserted ERR_NOTFOUND — which comes
     * back whether the click is rejected or not, because pulse B is missing
     * either way. Measured: with the run requirement removed to 1 the test still
     * passed. A guard shadowed by an earlier failure proves only the earlier
     * one. */
    memcpy(disc, sig, (size_t)N * 2 * sizeof(int16_t));
    disc[(size_t)(ACCUDISC_WOFF_PULSE_A - 4000) * 2] = 32767;
    disc[(size_t)(ACCUDISC_WOFF_PULSE_A - 4000) * 2 + 1] = 32767;
    info = (accudisc_write_offset_info)ACCUDISC_WRITE_OFFSET_INFO_INIT;
    assert(accudisc_write_offset_locate(disc, N, 0, &info) == ACCUDISC_OK);
    assert(info.write_offset == 0);
    assert(info.found_a == (int32_t)ACCUDISC_WOFF_PULSE_A);

    /* --- a defective disc is refused, not averaged ------------------------ */
    memcpy(disc, sig, (size_t)N * 2 * sizeof(int16_t));
    /* move ONLY pulse B, by clearing it and rewriting 50 samples later */
    memset(&disc[(size_t)ACCUDISC_WOFF_PULSE_B * 2], 0,
           (size_t)ACCUDISC_WOFF_PULSE_LEN * 2 * sizeof(int16_t));
    memcpy(&disc[(size_t)(ACCUDISC_WOFF_PULSE_B + 50) * 2],
           &sig[(size_t)ACCUDISC_WOFF_PULSE_B * 2],
           (size_t)ACCUDISC_WOFF_PULSE_LEN * 2 * sizeof(int16_t));
    info = (accudisc_write_offset_info)ACCUDISC_WRITE_OFFSET_INFO_INIT;
    assert(accudisc_write_offset_locate(disc, N, 0, &info)
           == ACCUDISC_ERR_AMBIGUOUS);
    assert(info.flags & ACCUDISC_WOFF_F_INCONSISTENT);
    assert(info.write_offset == ACCUDISC_OFFSET_NONE); /* never a guess */
    assert(info.offset_a == 0 && info.offset_b == 50); /* both still reported */

    /* --- argument and ABI guards ----------------------------------------- */
    info = (accudisc_write_offset_info)ACCUDISC_WRITE_OFFSET_INFO_INIT;
    assert(accudisc_write_offset_locate(NULL, N, 0, &info) == ACCUDISC_ERR_INVAL);
    assert(accudisc_write_offset_locate(sig, N, 0, NULL) == ACCUDISC_ERR_INVAL);
    assert(accudisc_write_offset_locate(sig, N - 1, 0, &info)
           == ACCUDISC_ERR_INVAL);
    info.size = 0;
    assert(accudisc_write_offset_locate(sig, N, 0, &info) == ACCUDISC_ERR_ABI);
    info.size = sizeof(info) + 1;
    assert(accudisc_write_offset_locate(sig, N, 0, &info) == ACCUDISC_ERR_ABI);

    /* --- THRESHOLD, NOT CORRELATION: a FOREIGN waveform must locate ------
     *
     * The interoperability promise in write_offset.c's header — "a
     * cdda2img-burnt disc is readable here and vice versa" — rests entirely on
     * the locator keying off LOUDNESS at the documented positions rather than
     * off the bytes our own generator emits. Nothing in the rest of this file
     * can tell the two apart: every buffer above is built from
     * accudisc_write_offset_signal, so a correlation-based locator matched to
     * that exact signal would pass all of them.
     *
     * Confirmed on real hardware 2026-08-27 (cdda2img ran this locator against
     * a PX-716A read-back of a disc burnt from THEIR generator and got -30,
     * agreeing with their tool on the offset and on both absolute positions).
     * Pinned here so the property cannot be lost to a future "optimisation"
     * that correlates against a known burst — which would look faster, pass
     * every other test, and silently stop reading other tools' discs.
     *
     * The foreign burst below is deliberately NOT ours: full scale rather than
     * half, a different generator, and NO forced full-scale leading edge.
     */
    {
        int16_t *foreign = malloc((size_t)N * 2 * sizeof(int16_t));
        const uint32_t pos[2] = { ACCUDISC_WOFF_PULSE_A, ACCUDISC_WOFF_PULSE_B };
        accudisc_write_offset_info fi = ACCUDISC_WRITE_OFFSET_INFO_INIT;
        uint32_t st = 12345u, k, i;
        int differs = 0;

        assert(foreign);
        memset(foreign, 0, (size_t)N * 2 * sizeof(int16_t));
        for (k = 0; k < 2; k++)
            for (i = 0; i < ACCUDISC_WOFF_PULSE_LEN; i++) {
                /* Any loud noise will do — that is the whole claim. */
                st = st * 1103515245u + 12345u;
                foreign[(pos[k] + i) * 2]     = (int16_t)(st >> 16);
                st = st * 1103515245u + 12345u;
                foreign[(pos[k] + i) * 2 + 1] = (int16_t)(st >> 16);
            }

        /* It really is a different signal, or the test below proves nothing. */
        for (i = 0; i < ACCUDISC_WOFF_PULSE_LEN * 2; i++)
            if (foreign[pos[0] * 2 + i] != sig[pos[0] * 2 + i]) { differs = 1; break; }
        assert(differs && "the foreign burst must not be our own waveform");

        assert(accudisc_write_offset_locate(foreign, N, 0, &fi) == ACCUDISC_OK);
        assert(fi.write_offset == 0);
        assert(fi.found_a == (int32_t)ACCUDISC_WOFF_PULSE_A);
        assert(fi.found_b == (int32_t)ACCUDISC_WOFF_PULSE_B);

        /* And it measures a real offset on that foreign signal, not merely
         * "finds something at zero". */
        shift_into(disc, foreign, -30);
        fi = (accudisc_write_offset_info)ACCUDISC_WRITE_OFFSET_INFO_INIT;
        assert(accudisc_write_offset_locate(disc, N, 0, &fi) == ACCUDISC_OK);
        assert(fi.write_offset == -30);

        free(foreign);
    }

    free(sig);
    free(disc);
    printf("test_write_offset: ok\n");
    /* --- THE FORCED LEADING EDGE, and why a comment was not enough --------
     *
     * Both bursts in the signal are the SAME waveform, written at two
     * positions. A quiet leading sample pair would therefore bias pulse A and
     * pulse B identically, off_a == off_b would still hold, and the two-pulse
     * consistency check would PASS with both values one sample late. That check
     * tests the disc, not the signal.
     *
     * So the forced full-scale first pair is the only thing standing between
     * this measurement and a silent +/-1, and it looks exactly like a redundant
     * line someone could tidy away. Pinned here so they cannot.
     *
     * Rate, for scale: samples are uniform on [-16384, 16383] and the locator
     * tests |L| > 500 OR |R| > 500, so a pair is quiet with probability
     * (1001/32768)^2 = 1 in 1072 — measured 1 in 1068 over 2e7 draws. Roughly
     * one seed in a thousand would be silently wrong without this.
     */
    {
        int16_t *sig = malloc((size_t)ACCUDISC_WOFF_SAMPLES * 2 * sizeof *sig);
        const uint32_t pa = ACCUDISC_WOFF_PULSE_A, pb = ACCUDISC_WOFF_PULSE_B;
        uint32_t i;
        int same = 1;

        assert(sig);
        assert(accudisc_write_offset_signal(sig, ACCUDISC_WOFF_SAMPLES)
               == ACCUDISC_OK);

        /* Full scale, both channels, at the first sample of BOTH bursts. */
        assert(sig[pa * 2] == 32767 && sig[pa * 2 + 1] == -32768);
        assert(sig[pb * 2] == 32767 && sig[pb * 2 + 1] == -32768);

        /* The premise of the paragraph above, asserted rather than assumed: if
         * a later change gave the two bursts independent noise, the consistency
         * check WOULD catch a leading-edge failure and this pin could relax.
         * While they are identical, it cannot. */
        for (i = 0; i < ACCUDISC_WOFF_PULSE_LEN * 2; i++)
            if (sig[pa * 2 + i] != sig[pb * 2 + i]) { same = 0; break; }
        assert(same && "the two bursts are one waveform — see write_offset.c");

        /* And the edge is genuinely doing work: nothing else in the burst is
         * guaranteed loud, so the assertion above is not just restating noise
         * that happened to be loud anyway. */
        {
            unsigned quiet = 0;
            for (i = 1; i < ACCUDISC_WOFF_PULSE_LEN; i++) {
                int l = sig[(pa + i) * 2], r = sig[(pa + i) * 2 + 1];

                if (l < 0) l = -l;
                if (r < 0) r = -r;
                if (!(l > 500 || r > 500))
                    quiet++;
            }
            /* This seed happens to have none, which is exactly why the forced
             * edge cannot be justified by inspecting this seed. */
            (void)quiet;
        }
        free(sig);
    }

    return 0;
}
