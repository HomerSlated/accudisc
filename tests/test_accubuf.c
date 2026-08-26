/* AccuBuffer tests. Device-free: the ring is exercised directly, with a fake
 * producer standing in for the engine, so every property below is checked at
 * the desk rather than inferred from a rip.
 *
 * The property that matters is OVERLAP. A ring that compiles, queues and
 * delivers in order still does nothing if the producer waits for the consumer
 * — and it would pass every ordering and integrity test in that state. So the
 * central test measures wall-clock: a producer pushing N chunks into a ring of
 * N against a sink that sleeps must finish in far less than N sleeps, because
 * the sink's time is supposed to be hidden. That is the only assertion here
 * that would fail if the ring were quietly synchronous. */

#include <string.h>
#include <time.h>
#include <stdlib.h>

#include <assert.h>

#include "read/accubuf.h"

#define SECTOR 2352u
#define NSEC   4u
#define SLOT   (NSEC * SECTOR)

static double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void nap_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

/* ---- ordering and payload integrity ------------------------------------- */

struct seen {
    unsigned n;
    uint32_t lba[64];
    uint8_t first[64];   /* first payload byte, to prove the copy is per-slot */
    int out_of_order;
};

static int sink_record(void *user, const accudisc_chunk *c)
{
    struct seen *s = user;

    if (s->n && c->lba <= s->lba[s->n - 1])
        s->out_of_order = 1;
    if (s->n < 64) {
        s->lba[s->n] = c->lba;
        s->first[s->n] = ((const uint8_t *)c->data)[0];
    }
    s->n++;
    return 0;
}

static void push_n(struct adsc_accubuf *ab, unsigned n, uint8_t *scratch)
{
    for (unsigned i = 0; i < n; i++) {
        accudisc_chunk c = { .lba = 1000 + i, .nsec = NSEC, .data = scratch,
                             .sector_len = SECTOR, .audio_len = SECTOR };

        memset(scratch, (uint8_t)(i + 1), SLOT);
        assert(adsc_accubuf_push(ab, &c) == 0 && "push");
    }
}

static void test_order_and_payload(void)
{
    struct adsc_accubuf ab;
    struct seen s;
    uint8_t *scratch = malloc(SLOT);

    memset(&s, 0, sizeof s);
    assert(adsc_accubuf_start(&ab, 4 * SLOT, SLOT, sink_record, &s)
                == ACCUDISC_OK && "start");
    push_n(&ab, 20, scratch);
    assert(adsc_accubuf_stop(&ab, 0) == 0 && "stop drains cleanly");

    assert(s.n == 20 && "every chunk was delivered");
    assert(!s.out_of_order && "chunks arrive in LBA order");
    for (unsigned i = 0; i < 20; i++) {
        assert(s.lba[i] == 1000 + i && "LBA sequence intact");
        /* THE COPY. The producer reuses one scratch buffer, overwriting it
         * before the consumer can possibly have drained the previous chunk.
         * If push referenced rather than copied, every delivered chunk would
         * carry the LAST value written. */
        assert(s.first[i] == (uint8_t)(i + 1) && "payload was copied into its own slot, not referenced");
    }
    free(scratch);
}

/* ---- the one that matters: does it actually overlap? --------------------- */

static int sink_slow(void *user, const accudisc_chunk *c)
{
    (void)c;
    (*(unsigned *)user)++;
    nap_ms(20);
    return 0;
}

static void test_producer_is_not_blocked_by_the_sink(void)
{
    struct adsc_accubuf ab;
    unsigned got = 0;
    uint8_t *scratch = malloc(SLOT);
    double t0, push_secs, total_secs;

    /* 8 slots, 8 chunks, a sink that costs 20 ms each. Synchronous delivery
     * would make the pushes take ~160 ms. Overlapped, the ring swallows all
     * eight and the producer returns almost immediately. */
    assert(adsc_accubuf_start(&ab, 8 * SLOT, SLOT, sink_slow, &got)
                == ACCUDISC_OK && "start");
    t0 = now_s();
    push_n(&ab, 8, scratch);
    push_secs = now_s() - t0;
    assert(adsc_accubuf_stop(&ab, 0) == 0 && "stop");
    total_secs = now_s() - t0;

    assert(got == 8 && "the sink saw all eight");
    /* Generous by 4x against the 160 ms a synchronous path would cost: this
     * has to be robust on a loaded machine, and it only needs to separate
     * "overlapped" from "not overlapped", not to measure the margin. */
    assert(push_secs < 0.040 && "producer returned without waiting for the sink");
    /* And the work really did happen, rather than being skipped. */
    assert(total_secs > 0.120 && "the sink's time was actually spent");
    free(scratch);
}

static void test_backpressure_is_bounded(void)
{
    struct adsc_accubuf ab;
    unsigned got = 0;
    uint8_t *scratch = malloc(SLOT);

    /* 2 slots against 6 chunks and a slow sink: the producer MUST block, and
     * the ring must never exceed its capacity. An unbounded buffer would be a
     * different bug — the one that swaps mid-burn. */
    assert(adsc_accubuf_start(&ab, 2 * SLOT, SLOT, sink_slow, &got)
                == ACCUDISC_OK && "start");
    push_n(&ab, 6, scratch);
    assert(adsc_accubuf_stop(&ab, 0) == 0 && "stop");

    assert(got == 6 && "all six delivered despite the squeeze");
    assert(ab.peak_count <= 2 && "never exceeded capacity");
    assert(ab.stalls > 0 && "the producer really did have to wait");
    free(scratch);
}

/* ---- refusal and cancellation ------------------------------------------- */

static int sink_refuse(void *user, const accudisc_chunk *c)
{
    (void)c;
    return ++(*(unsigned *)user) >= 3 ? -7 : 0;
}

static void test_sink_refusal_reaches_the_producer(void)
{
    struct adsc_accubuf ab;
    unsigned n = 0;
    uint8_t *scratch = malloc(SLOT);
    int saw_refusal = 0;

    assert(adsc_accubuf_start(&ab, 2 * SLOT, SLOT, sink_refuse, &n)
                == ACCUDISC_OK && "start");
    for (unsigned i = 0; i < 40; i++) {
        accudisc_chunk c = { .lba = i, .nsec = NSEC, .data = scratch,
                             .sector_len = SECTOR, .audio_len = SECTOR };

        if (adsc_accubuf_push(&ab, &c) != 0) { saw_refusal = 1; break; }
    }
    /* The refusal has to travel BACKWARDS across the thread boundary. Without
     * the ring the sink's return value was the producer's own; with it, a
     * producer that never checked would read the whole disc after the caller
     * had asked it to stop. */
    assert(saw_refusal && "push reported the sink's refusal");
    assert(adsc_accubuf_stop(&ab, 1) == -7 && "stop returns the sink's own code, not a substitute");
    free(scratch);
}

static void test_stop_on_a_zeroed_struct_is_safe(void)
{
    struct adsc_accubuf ab;

    /* The engine zeroes it and may `goto out` before ever starting it — an
     * allocation failure does exactly that. Stop must be a no-op there. */
    memset(&ab, 0, sizeof ab);
    assert(adsc_accubuf_stop(&ab, 0) == 0 && "stop on unstarted ring");
    assert(adsc_accubuf_stop(&ab, 1) == 0 && "and again, discarding");
}

static void test_sizing_rounds_down_and_clamps(void)
{
    struct adsc_accubuf ab;
    struct seen s;

    memset(&s, 0, sizeof s);
    /* Below one slot must still yield a usable ring, not zero slots: a
     * caller passing --buffer 1 gets the minimum, not a division by zero. */
    assert(adsc_accubuf_start(&ab, 1, SLOT, sink_record, &s)
                == ACCUDISC_OK && "tiny request");
    assert(ab.nslots == ADSC_ACCUBUF_MIN_SLOTS && "clamped up to the floor");
    assert(adsc_accubuf_stop(&ab, 0) == 0 && "stop");

    /* And an absurd request is clamped rather than attempted: 4 GiB of ring
     * would otherwise be malloc'd, succeed on an overcommitting kernel, and
     * fail later at the memset — which is exactly the failure the touch-at-
     * allocation is there to convert into an honest startup error. */
    assert(adsc_accubuf_start(&ab, (size_t)1 << 34, SLOT, sink_record, &s)
                == ACCUDISC_OK && "absurd request");
    assert(ab.nslots == ADSC_ACCUBUF_MAX_SLOTS && "clamped to the ceiling");
    assert(adsc_accubuf_stop(&ab, 0) == 0 && "stop");
}

int main(void)
{
    test_order_and_payload();
    test_producer_is_not_blocked_by_the_sink();
    test_backpressure_is_bounded();
    test_sink_refusal_reaches_the_producer();
    test_stop_on_a_zeroed_struct_is_safe();
    test_sizing_rounds_down_and_clamps();
    return 0;
}
