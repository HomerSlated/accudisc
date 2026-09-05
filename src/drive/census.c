/* Counter census: read a span with the vendor C1/C2/CU counters armed, sampling
 * them at a fixed cadence.
 *
 * This exists as one call rather than three because the counters are ARMED DRIVE
 * STATE. Every path out of the scan has to disarm them — the early return when
 * the range is empty, a counter read that fails, a caller cancelling from the
 * callback. The CLI got this right by hand and had to remember to; a binding
 * author would have to remember to as well, in a language where the natural
 * shape is a generator that can be abandoned half-way. Same discipline as
 * accudisc_speed_uncap_push/pop: whoever changes drive state owns putting it
 * back, on every path.
 *
 * See API_PLAN.md §5.3.
 */

#include <string.h>

#include "../internal.h"

int accudisc_counter_census(accudisc_device *dev,
                            const accudisc_census_opts *opts,
                            accudisc_census_fn fn, void *user,
                            accudisc_census_stats *stats)
{
    accudisc_census_stats st;
    uint32_t cadence;
    int rc, ret = ACCUDISC_OK;

    memset(&st, 0, sizeof(st));
    if (stats)
        *stats = st;

    if (!dev || !opts || !fn)
        return ACCUDISC_ERR_INVAL;

    /* Before the span check, so a stale binding is diagnosed as ERR_ABI
     * ("rebuild") rather than ERR_INVAL ("fix your arguments"). */
    accudisc_census_opts local;
    {
        int abi = adsc_abi_import(&local, sizeof local, opts, opts->size);
        if (abi != ACCUDISC_OK)
            return abi;
        opts = &local;
    }

    if (opts->end <= opts->start)
        return ACCUDISC_ERR_INVAL; /* empty span: nothing to arm for */

    cadence = opts->cadence ? opts->cadence : ACCUDISC_CENSUS_CADENCE;

    /* Arm last, after every argument check, so a rejected call never leaves the
     * drive armed. */
    rc = accudisc_counter_scan_begin(dev);
    if (rc != ACCUDISC_OK)
        return rc;

    if (opts->speed_x)
        accudisc_set_speed(dev, opts->speed_x);

    for (uint32_t lba = opts->start; lba < opts->end; lba += cadence) {
        accudisc_census_sample s;
        accudisc_read_req req;
        uint32_t remain = opts->end - lba;

        if (opts->cancel && *opts->cancel) {
            ret = ACCUDISC_ERR_CANCELLED;
            break;
        }

        memset(&s, 0, sizeof(s));
        s.lba = lba;
        s.count = remain < cadence ? remain : cadence;

        memset(&req, 0, sizeof(req));
        req.size = sizeof(req);
        req.lba = lba;
        req.count = s.count;
        req.retries = 1; /* one attempt: a census maps damage, it does not
                          * rescue it, and rereads would corrupt the counter
                          * interval this sample is measuring */
        s.read_err = accudisc_read_cdda(dev, &req, NULL, NULL, NULL);

        /* A failed read is data, not an abort — an unreadable span is exactly
         * what a census is looking for. The counters are still read so the
         * sample keeps its place in the map. */
        rc = accudisc_counter_scan_read(dev, &s.counters);
        if (rc != ACCUDISC_OK) {
            ret = rc; /* the counters themselves failed: the census is over */
            break;
        }

        st.samples++;
        if (s.read_err != ACCUDISC_OK)
            st.read_errors++;
        st.c1 += s.counters.c1;
        st.c2 += s.counters.c2;
        st.cu += s.counters.cu;
        if (s.counters.c1 > st.peak_c1)
            st.peak_c1 = s.counters.c1;
        if (s.counters.c2 > st.peak_c2)
            st.peak_c2 = s.counters.c2;
        if (s.counters.cu > st.peak_cu)
            st.peak_cu = s.counters.cu;

        /* Check the drive's own C1 total against the sum of its three C1
         * severity counts. Every C1 block error carries one, two or three bad
         * symbols, so the identity holds by construction and a violation
         * means the byte frame we decode is not the frame the firmware fills.
         * Free to test, and the only way we would ever find out.
         *
         * Samples without detail are not counted on EITHER side. A drive with
         * no vendor counters leaves every e-field zero, and 0 == 0+0+0 would
         * otherwise score as a passing check on evidence that distinguishes
         * nothing — the exact shape of a test that is green while measuring
         * nothing. */
        if (s.counters.have_detail) {
            st.bler_checked++;
            if (s.counters.bler !=
                s.counters.e11 + s.counters.e21 + s.counters.e31)
                st.bler_mismatch++;
        }

        if (fn(&s, user) != 0) {
            ret = ACCUDISC_ERR_CANCELLED;
            break;
        }
    }

    accudisc_counter_scan_end(dev);

    if (stats)
        *stats = st;
    return ret;
}
