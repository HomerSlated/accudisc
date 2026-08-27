/* Write FIFO implementation. Rationale and contract live in fifo.h. */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fifo.h"

#define SECTOR 2352u
/* CD-DA at 1x. The unit the duration form converts through. */
#define CDDA_BYTES_PER_SEC (SECTOR * 75u)

static void byteswap16(uint8_t *p, size_t bytes)
{
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        uint8_t t = p[i];
        p[i] = p[i + 1];
        p[i + 1] = t;
    }
}

size_t adsc_wfifo_bytes_for(double seconds, unsigned speed_x, size_t cap)
{
    double want;

    if (speed_x == 0)
        speed_x = 1;
    if (seconds < 0)
        seconds = 0;
    want = seconds * (double)CDDA_BYTES_PER_SEC * (double)speed_x;
    /* The clamp is not a nicety. Five seconds at 48x is ~42 MB of LOCKED
     * memory, which on a small machine is a refusal to start rather than a
     * buffer. The caller reports when this bit, because silently giving 0.9 s
     * and calling it 5 s is the failure this project keeps meeting. */
    if (want > (double)cap)
        return cap;
    return (size_t)want;
}

/* The producer. Walks the segment list in order, filling slots. */
static void *fill(void *arg)
{
    struct adsc_wfifo *f = arg;
    unsigned si = 0;
    uint32_t done_in_seg = 0;

    for (;;) {
        uint32_t n, want;
        uint64_t off;
        uint8_t *dst;
        ssize_t got;

        pthread_mutex_lock(&f->mtx);
        while (f->count == f->nslots && !f->cancelled) {
            f->producer_waits++;
            pthread_cond_wait(&f->not_full, &f->mtx);
        }
        if (f->cancelled) { pthread_mutex_unlock(&f->mtx); return NULL; }
        dst = f->arena + (size_t)f->head * f->slot_bytes;
        pthread_mutex_unlock(&f->mtx);

        /* Advance to the next segment with data left. */
        while (si < f->nseg && done_in_seg >= f->seg[si].sectors) {
            si++;
            done_in_seg = 0;
        }
        if (si >= f->nseg) {                    /* end of stream */
            pthread_mutex_lock(&f->mtx);
            f->producer_done = 1;
            pthread_cond_broadcast(&f->not_empty);
            pthread_mutex_unlock(&f->mtx);
            return NULL;
        }

        want = f->seg[si].sectors - done_in_seg;
        n = want < f->sectors_per_slot ? want : f->sectors_per_slot;
        off = f->seg[si].file_offset + (uint64_t)done_in_seg * SECTOR;

        got = pread(f->fd, dst, (size_t)n * SECTOR, (off_t)off);
        if (got != (ssize_t)((size_t)n * SECTOR)) {
            pthread_mutex_lock(&f->mtx);
            if (!f->producer_rc)
                f->producer_rc = ACCUDISC_ERR_IO;
            f->producer_done = 1;
            pthread_cond_broadcast(&f->not_empty);
            pthread_mutex_unlock(&f->mtx);
            return NULL;
        }
        /* Byteswap HERE, on the producer thread. It is the one piece of CPU
         * work in the pipeline, and doing it on the consumer would put it back
         * on the path between the ring and the drive — undoing the decoupling
         * the ring exists for. */
        if (f->byteswap)
            byteswap16(dst, (size_t)n * SECTOR);

        done_in_seg += n;

        pthread_mutex_lock(&f->mtx);
        f->slot_nsec[f->head] = n;
        f->head = (f->head + 1) % f->nslots;
        f->count++;
        pthread_cond_signal(&f->not_empty);
        pthread_mutex_unlock(&f->mtx);
    }
}

int adsc_wfifo_start(struct adsc_wfifo *f, size_t bytes, uint32_t sectors_per_slot,
                     int fd, const struct adsc_wfifo_seg *seg, unsigned nseg,
                     int byteswap)
{
    uint64_t want;

    if (!f || !seg || nseg == 0 || sectors_per_slot == 0 || fd < 0)
        return ACCUDISC_ERR_INVAL;
    memset(f, 0, sizeof(*f));

    f->slot_bytes = (size_t)sectors_per_slot * SECTOR;
    want = bytes / f->slot_bytes;
    if (want < ADSC_WFIFO_MIN_SLOTS)
        want = ADSC_WFIFO_MIN_SLOTS;
    if (want > ADSC_WFIFO_MAX_SLOTS)
        want = ADSC_WFIFO_MAX_SLOTS;
    f->nslots = (uint32_t)want;
    f->arena_bytes = (size_t)f->nslots * f->slot_bytes;
    f->sectors_per_slot = sectors_per_slot;
    f->fd = fd;
    f->seg = seg;
    f->nseg = nseg;
    f->byteswap = byteswap;
    f->min_count = f->nslots;

    f->slot_nsec = calloc(f->nslots, sizeof(*f->slot_nsec));
    f->arena = malloc(f->arena_bytes);
    if (!f->slot_nsec || !f->arena) {
        free(f->slot_nsec);
        free(f->arena);
        memset(f, 0, sizeof(*f));
        return ACCUDISC_ERR_NOMEM;
    }
    /* Touch every page NOW, then pin them. malloc hands back address space;
     * without the touch the shortfall surfaces as a fault partway through
     * something unrepeatable. Without the pin, memory pressure evicts the far
     * end of the ring during exactly the stall it exists to absorb. */
    memset(f->arena, 0, f->arena_bytes);
    f->locked = (mlock(f->arena, f->arena_bytes) == 0);

    if (pthread_mutex_init(&f->mtx, NULL) != 0)
        goto fail_alloc;
    if (pthread_cond_init(&f->not_empty, NULL) != 0)
        goto fail_mtx;
    if (pthread_cond_init(&f->not_full, NULL) != 0)
        goto fail_empty;
    if (pthread_create(&f->thread, NULL, fill, f) != 0)
        goto fail_full;
    f->thread_live = 1;
    return ACCUDISC_OK;

fail_full:
    pthread_cond_destroy(&f->not_full);
fail_empty:
    pthread_cond_destroy(&f->not_empty);
fail_mtx:
    pthread_mutex_destroy(&f->mtx);
fail_alloc:
    if (f->locked)
        munlock(f->arena, f->arena_bytes);
    free(f->slot_nsec);
    free(f->arena);
    memset(f, 0, sizeof(*f));
    return ACCUDISC_ERR_NOMEM;
}

int adsc_wfifo_prefill(struct adsc_wfifo *f, unsigned *slots_out)
{
    if (!f || !f->thread_live)
        return ACCUDISC_ERR_INVAL;

    pthread_mutex_lock(&f->mtx);
    while (f->count < f->nslots && !f->producer_done && !f->producer_rc)
        pthread_cond_wait(&f->not_empty, &f->mtx);
    if (slots_out)
        *slots_out = f->count;
    /* The startup pop would otherwise be counted as a starvation, and on a
     * drive with no failover that single false positive would stop EVERY burn
     * at sector 0 — the buffer making burning impossible on exactly the drives
     * it exists to protect. Measured on the PX-716A before this existed:
     * "FIFO empty at sector 0", low-water 1 of 111 slots. */
    f->min_count = f->count;
    pthread_mutex_unlock(&f->mtx);
    return f->producer_rc ? f->producer_rc : ACCUDISC_OK;
}

int adsc_wfifo_pop(struct adsc_wfifo *f, const uint8_t **data, int *was_empty)
{
    uint32_t n;
    int rc = 0;

    *was_empty = 0;
    pthread_mutex_lock(&f->mtx);
    if (f->count == 0 && !f->producer_done) {
        /* THE HOST HAS FALLEN BEHIND. Recorded before waiting, because the
         * fact that we had to wait at all is the signal — how long is a
         * secondary question, and by the time the wait returns the evidence
         * that it happened would otherwise be gone. */
        *was_empty = 1;
        f->starved++;
    }
    while (f->count == 0 && !f->producer_done)
        pthread_cond_wait(&f->not_empty, &f->mtx);

    if (f->count == 0) {                 /* drained and the producer finished */
        rc = f->producer_rc ? f->producer_rc : 0;
        pthread_mutex_unlock(&f->mtx);
        return rc;                       /* 0 sectors = end of stream */
    }
    /* Only while the source still has data. Once the producer is done the ring
     * drains to empty because the FILE ended, not because the host lost — and
     * recording that as the low-water mark reports "we nearly starved" about
     * the most normal event in a burn. Measured before this guard: 0
     * starvations and a low-water of 1/111 on a run where the producer spent
     * the whole burn blocked on a FULL ring. */
    if (!f->producer_done && f->count < f->min_count)
        f->min_count = f->count;
    n = f->slot_nsec[f->tail];
    *data = f->arena + (size_t)f->tail * f->slot_bytes;
    pthread_mutex_unlock(&f->mtx);
    return (int)n;
}

/* Release the slot the previous pop handed out. Separate from pop so the
 * consumer can WRITE from the ring without copying — the slot stays valid
 * until the drive has taken it, which is what keeps one memcpy out of the
 * burn path entirely. */
void adsc_wfifo_release(struct adsc_wfifo *f)
{
    pthread_mutex_lock(&f->mtx);
    f->tail = (f->tail + 1) % f->nslots;
    f->count--;
    pthread_cond_signal(&f->not_full);
    pthread_mutex_unlock(&f->mtx);
}

int adsc_wfifo_stop(struct adsc_wfifo *f, int discard)
{
    int rc;

    if (!f || !f->thread_live)
        return 0;

    pthread_mutex_lock(&f->mtx);
    if (discard)
        f->cancelled = 1;
    pthread_cond_broadcast(&f->not_full);
    pthread_cond_broadcast(&f->not_empty);
    pthread_mutex_unlock(&f->mtx);

    pthread_join(f->thread, NULL);
    f->thread_live = 0;
    rc = f->producer_rc;

    pthread_cond_destroy(&f->not_full);
    pthread_cond_destroy(&f->not_empty);
    pthread_mutex_destroy(&f->mtx);
    if (f->locked)
        munlock(f->arena, f->arena_bytes);
    free(f->slot_nsec);
    free(f->arena);
    f->slot_nsec = NULL;
    f->arena = NULL;
    return rc;
}

uint32_t accudisc_fifo_bytes_for(double seconds, unsigned speed_x)
{
    return (uint32_t)adsc_wfifo_bytes_for(seconds, speed_x,
                                          ACCUDISC_FIFO_MAX_BYTES);
}
