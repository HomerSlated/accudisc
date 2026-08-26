/* AccuBuffer — a bounded chunk ring between the read engine and the caller's
 * sink, so that time spent in the sink is not time the drive is not being read.
 *
 * WHY IT EXISTS, and equally what it is NOT for. Measured 2026-08-26 against a
 * cgroup write-rate-capped container at half the drive's streaming rate: a
 * whole-disc read took 301.1 s against a pure-sink floor of 292.5 s, i.e. 95.5%
 * of the disc read was already hidden behind the sink. That is the page cache
 * doing this job for free — fwrite returns immediately and kworkers drain it
 * while the drive keeps streaming. For an ordinary buffered file sink a
 * user-space ring adds almost nothing, and claiming otherwise would be a
 * feature justified by a story rather than a measurement.
 *
 * What the page cache does NOT cover is work done INSIDE the sink callback, and
 * that is this library's own shape: accudisc_read_cdda hands each chunk to a
 * caller-supplied function. FLAC encoding, hashing, AccurateRip/CTDB
 * checksums, a GUI repaint — every millisecond there is a millisecond the drive
 * is not being read, with no kernel buffer in between. THAT is what this ring
 * decouples. Pipes, sockets, O_DIRECT and synchronous network filesystems are
 * the same shape.
 *
 * It cannot create bandwidth. A sink that is SUSTAINABLY slower than the drive
 * will fill any ring and then bound the read regardless; a ring converts a
 * BURST into a delay, nothing more. Sizing follows from that: capacity only has
 * to cover the longest stall worth surviving.
 *
 * THE SINK RUNS ON THE CONSUMER THREAD. That is the one consumer-visible
 * consequence and it is not negotiable — overlap is the entire point, and
 * overlap means another thread. See the header for the contract.
 *
 * Memory is a plain malloc: anonymous pages, RAM first and swap under pressure,
 * which is what "paged memory" already means on Linux. The ring is BOUNDED
 * anyway, deliberately, because relying on paging here would be self-defeating
 * — swap is slower than the sink being buffered for, so a ring that grew into
 * it would replace steady backpressure with an unpredictable page-fault stall
 * on the producer. Every slot is touched at allocation so a shortfall fails at
 * startup rather than at 60% of a rip. */

#ifndef ACCUDISC_READ_ACCUBUF_H
#define ACCUDISC_READ_ACCUBUF_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <accudisc/accudisc.h>

/* Fewest slots that can overlap anything: one being filled, one being drained.
 * A single slot is not a ring, it is a rename of the synchronous path. */
#define ADSC_ACCUBUF_MIN_SLOTS 2u
/* Enough slots that the bookkeeping is never the reason to stop. */
#define ADSC_ACCUBUF_MAX_SLOTS 4096u

struct adsc_accubuf_slot {
    accudisc_chunk meta;   /* .data points into this slot's payload */
    uint8_t *payload;
};

struct adsc_accubuf {
    struct adsc_accubuf_slot *slot;
    uint8_t *arena;             /* one allocation backing every payload */
    uint32_t nslots;
    size_t slot_bytes;          /* capacity of one payload */

    pthread_mutex_t mtx;
    pthread_cond_t not_empty;   /* producer -> consumer */
    pthread_cond_t not_full;    /* consumer -> producer */
    pthread_t thread;
    int thread_live;

    uint32_t head;              /* next slot the producer will fill */
    uint32_t tail;              /* next slot the consumer will drain */
    uint32_t count;             /* filled slots */

    accudisc_sink_fn sink;
    void *user;

    int producer_done;          /* no more chunks will be pushed */
    int sink_rc;                /* first non-zero return from the sink */

    /* Watermark, for reporting whether the ring was ever the constraint. A
     * ring that never filled did nothing, and saying so is more useful than a
     * throughput figure that cannot distinguish "helped" from "not needed". */
    uint32_t peak_count;
    uint64_t stalls;            /* times the producer had to wait for a slot */
};

/* Size and start the ring. `bytes` is the requested capacity; it is rounded
 * DOWN to whole slots of `slot_bytes` and clamped to [MIN_SLOTS, MAX_SLOTS].
 * Returns ACCUDISC_OK, or ACCUDISC_ERR_NOMEM / ACCUDISC_ERR_INVAL.
 *
 * A failure here is a FAILURE, never a silent fall back to the synchronous
 * path: a caller that asked for a buffer and got different behaviour without
 * being told is the shape of defect this project refuses. */
int adsc_accubuf_start(struct adsc_accubuf *ab, size_t bytes, size_t slot_bytes,
                       accudisc_sink_fn sink, void *user);

/* Copy one chunk into the ring, blocking while it is full. Returns 0 on
 * success, or the sink's non-zero return if the sink has already refused —
 * in which case nothing further is pushed. */
int adsc_accubuf_push(struct adsc_accubuf *ab, const accudisc_chunk *c);

/* Stop. `discard` non-zero abandons whatever is still queued (cancellation);
 * zero drains it first (normal completion). Joins the thread and frees
 * everything. Returns the sink's first non-zero return, or 0. Safe on a
 * zeroed struct that was never started. */
int adsc_accubuf_stop(struct adsc_accubuf *ab, int discard);

#endif /* ACCUDISC_READ_ACCUBUF_H */
