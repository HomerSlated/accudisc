/* AccuBuffer implementation. Rationale and contract live in accubuf.h. */

#include <string.h>
#include <stdlib.h>

#include "accubuf.h"

/* The consumer. One thread, so chunks reach the sink in exactly the order the
 * producer pushed them — LBA order, same as the synchronous path. A caller
 * cannot tell the difference from the sequence alone, only from the thread. */
static void *drain(void *arg)
{
    struct adsc_accubuf *ab = arg;

    for (;;) {
        struct adsc_accubuf_slot *s;
        accudisc_chunk c;
        int rc;

        pthread_mutex_lock(&ab->mtx);
        while (ab->count == 0 && !ab->producer_done)
            pthread_cond_wait(&ab->not_empty, &ab->mtx);
        if (ab->count == 0) { /* drained and the producer is finished */
            pthread_mutex_unlock(&ab->mtx);
            return NULL;
        }
        s = &ab->slot[ab->tail];
        /* Copy the metadata out under the lock, then call the sink WITHOUT it.
         * Holding the mutex across a caller callback would let an arbitrarily
         * slow consumer block the producer's push on the mutex rather than on
         * the not_full condition — the ring would still be full of free slots
         * and refuse to use them. */
        c = s->meta;
        pthread_mutex_unlock(&ab->mtx);

        rc = ab->sink(ab->user, &c);

        pthread_mutex_lock(&ab->mtx);
        ab->tail = (ab->tail + 1) % ab->nslots;
        ab->count--;
        if (rc != 0 && ab->sink_rc == 0)
            ab->sink_rc = rc;
        pthread_cond_signal(&ab->not_full);
        /* A refusal stops us consuming, but the slot count still had to be
         * decremented above or a producer blocked in push would never wake to
         * see the refusal. */
        if (rc != 0) {
            pthread_mutex_unlock(&ab->mtx);
            return NULL;
        }
        pthread_mutex_unlock(&ab->mtx);
    }
}

int adsc_accubuf_start(struct adsc_accubuf *ab, size_t bytes, size_t slot_bytes,
                       accudisc_sink_fn sink, void *user)
{
    uint64_t want;

    if (!ab || !sink || slot_bytes == 0)
        return ACCUDISC_ERR_INVAL;
    memset(ab, 0, sizeof(*ab));

    want = (uint64_t)bytes / slot_bytes;
    if (want < ADSC_ACCUBUF_MIN_SLOTS)
        want = ADSC_ACCUBUF_MIN_SLOTS;
    if (want > ADSC_ACCUBUF_MAX_SLOTS)
        want = ADSC_ACCUBUF_MAX_SLOTS;
    ab->nslots = (uint32_t)want;
    ab->slot_bytes = slot_bytes;
    ab->sink = sink;
    ab->user = user;

    ab->slot = calloc(ab->nslots, sizeof(*ab->slot));
    ab->arena = malloc((size_t)ab->nslots * slot_bytes);
    if (!ab->slot || !ab->arena) {
        free(ab->slot);
        free(ab->arena);
        memset(ab, 0, sizeof(*ab));
        return ACCUDISC_ERR_NOMEM;
    }
    /* Touch every page NOW. malloc hands back address space, not memory; on an
     * overcommitting kernel the shortfall surfaces as a fault (or the OOM
     * killer) at first write, which for a rip means partway through, and for a
     * burn means partway through something unrepeatable. Fail here instead. */
    memset(ab->arena, 0, (size_t)ab->nslots * slot_bytes);
    for (uint32_t i = 0; i < ab->nslots; i++)
        ab->slot[i].payload = ab->arena + (size_t)i * slot_bytes;

    if (pthread_mutex_init(&ab->mtx, NULL) != 0)
        goto fail_alloc;
    if (pthread_cond_init(&ab->not_empty, NULL) != 0)
        goto fail_mtx;
    if (pthread_cond_init(&ab->not_full, NULL) != 0)
        goto fail_empty;
    if (pthread_create(&ab->thread, NULL, drain, ab) != 0)
        goto fail_full;
    ab->thread_live = 1;
    return ACCUDISC_OK;

fail_full:
    pthread_cond_destroy(&ab->not_full);
fail_empty:
    pthread_cond_destroy(&ab->not_empty);
fail_mtx:
    pthread_mutex_destroy(&ab->mtx);
fail_alloc:
    free(ab->slot);
    free(ab->arena);
    memset(ab, 0, sizeof(*ab));
    return ACCUDISC_ERR_NOMEM;
}

int adsc_accubuf_push(struct adsc_accubuf *ab, const accudisc_chunk *c)
{
    size_t need = (size_t)c->nsec * c->sector_len;
    struct adsc_accubuf_slot *s;
    int rc;

    if (need > ab->slot_bytes)
        return ACCUDISC_ERR_INVAL; /* sized from the same numbers; cannot happen */

    pthread_mutex_lock(&ab->mtx);
    while (ab->count == ab->nslots && ab->sink_rc == 0) {
        ab->stalls++;
        pthread_cond_wait(&ab->not_full, &ab->mtx);
    }
    if ((rc = ab->sink_rc) != 0) {
        pthread_mutex_unlock(&ab->mtx);
        return rc; /* the sink refused; push nothing more */
    }
    s = &ab->slot[ab->head];
    /* The engine reuses its transfer buffer on the next iteration, so the
     * payload has to be COPIED rather than referenced. ~63 KiB per chunk at
     * ~73 chunks/s is 4.6 MB/s of memcpy against a 6.6 MB/s transfer — real,
     * and far cheaper than the stall it removes. */
    memcpy(s->payload, c->data, need);
    s->meta = *c;
    s->meta.data = s->payload;
    ab->head = (ab->head + 1) % ab->nslots;
    ab->count++;
    if (ab->count > ab->peak_count)
        ab->peak_count = ab->count;
    pthread_cond_signal(&ab->not_empty);
    pthread_mutex_unlock(&ab->mtx);
    return 0;
}

int adsc_accubuf_stop(struct adsc_accubuf *ab, int discard)
{
    int rc;

    if (!ab || !ab->thread_live)
        return 0;

    pthread_mutex_lock(&ab->mtx);
    ab->producer_done = 1;
    if (discard) {
        /* Cancellation: abandon what is queued. Documented, because with a
         * buffer in play the engine may be several chunks ahead of the sink
         * when the read is aborted, and those chunks are simply never
         * delivered. The alternative — draining on cancel — would make a
         * cancel take as long as the backlog, which is the opposite of what a
         * caller asking to stop wants. */
        ab->count = 0;
        ab->tail = ab->head;
    }
    pthread_cond_broadcast(&ab->not_empty);
    pthread_cond_broadcast(&ab->not_full);
    pthread_mutex_unlock(&ab->mtx);

    pthread_join(ab->thread, NULL);
    ab->thread_live = 0;
    rc = ab->sink_rc;

    pthread_cond_destroy(&ab->not_full);
    pthread_cond_destroy(&ab->not_empty);
    pthread_mutex_destroy(&ab->mtx);
    free(ab->slot);
    free(ab->arena);
    ab->slot = NULL;
    ab->arena = NULL;
    return rc;
}
