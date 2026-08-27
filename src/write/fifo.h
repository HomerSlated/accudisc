/* The write FIFO — a bounded, memory-LOCKED ring between the source file and
 * the drive, so that a stall on the host side is absorbed instead of reaching
 * the laser.
 *
 * WHY THIS EXISTS, and why the measurement that said it did not is wrong.
 * Timing a burn on an idle machine says the host feeds far faster than the
 * drive writes: pread costs ~0.4% of a burn, and the drive's own buffer holds
 * seconds. Concluding from that that a FIFO is unnecessary is a category error
 * — it measures the steady state and reasons about the tail. A buffer exists
 * for the event that has not happened yet. This machine has twice been driven
 * into a multi-second stall by filling a tmpfs; every burning application in
 * the last thirty years ships one of these (Nero's UltraBuffer, cdrecord's
 * FIFO, cdrdao's --buffers), and they do not ship it because their authors
 * measured a quiet afternoon.
 *
 * MEMORY-LOCKED, AND THAT IS THE POINT rather than a refinement. Under memory
 * pressure the kernel evicts the least-recently-touched pages, which in a burn
 * FIFO is the far end — the part not yet consumed. An unlocked ring would take
 * a major fault mid-burn, during exactly the event it exists to survive, and
 * would be worse than no ring at all because it would have promised protection.
 * mlock failure is REPORTED and the burn continues unlocked; a caller that
 * asked for a buffer and silently got something weaker is the shape this
 * project refuses (see read/accubuf.h, same rule).
 *
 * IT CANNOT CREATE BANDWIDTH. A source sustainably slower than the drive fills
 * any ring and then bounds the burn regardless; a ring converts a BURST into a
 * delay. Sizing follows from that: capacity covers the longest stall worth
 * surviving, which is why it is expressed in SECONDS of audio rather than
 * bytes.
 *
 * WHEN IT EMPTIES, what happens depends on whether anything is behind us —
 * see adsc_wfifo_pop and the burn engine's underrun policy. */

#ifndef ACCUDISC_WRITE_FIFO_H
#define ACCUDISC_WRITE_FIFO_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <accudisc/accudisc.h>

/* Fewest slots that can overlap anything: one filling, one draining. */
#define ADSC_WFIFO_MIN_SLOTS 2u
/* Ceiling on slot COUNT, independent of the byte ceiling. */
#define ADSC_WFIFO_MAX_SLOTS 65536u

/* One contiguous run of the source file: a track. The producer walks these in
 * order, so the ring carries the burn's audio in LBA order and the consumer
 * never has to know where a track boundary fell. */
struct adsc_wfifo_seg {
    uint64_t file_offset;
    uint32_t sectors;
};

struct adsc_wfifo {
    uint8_t *arena;             /* one allocation, mlock'd where possible */
    size_t   arena_bytes;
    uint32_t nslots;
    size_t   slot_bytes;        /* capacity of one slot's payload */
    uint32_t *slot_nsec;        /* sectors actually in each slot */

    pthread_mutex_t mtx;
    pthread_cond_t  not_empty;  /* producer -> consumer */
    pthread_cond_t  not_full;   /* consumer -> producer */
    pthread_t       thread;
    int             thread_live;

    uint32_t head, tail, count;

    /* the source */
    int       fd;
    const struct adsc_wfifo_seg *seg;
    unsigned  nseg;
    uint32_t  sectors_per_slot;
    int       byteswap;

    int producer_done;          /* no more data will arrive */
    int producer_rc;            /* first error from the reader */
    int cancelled;              /* consumer asked the producer to stop */
    int locked;                 /* mlock succeeded */

    /* Reporting. min_count is the low-water mark: how close the ring came to
     * empty, which is the host-side twin of the drive's buffer fill and the
     * only evidence of whether the FIFO ever actually did anything. */
    uint32_t min_count;
    uint64_t starved;           /* times the consumer found it EMPTY */
    uint64_t producer_waits;    /* times the producer found it FULL */
};

/* Size and start the ring, and begin filling it immediately.
 *
 * `bytes` is the requested capacity, rounded DOWN to whole slots and clamped
 * to [MIN_SLOTS, MAX_SLOTS]. `sectors_per_slot` must match the chunk size the
 * consumer will pop with.
 *
 * A failure here is a FAILURE, never a silent fall back to the synchronous
 * path: a caller that asked for a buffer and got different behaviour without
 * being told is the shape this project refuses. mlock failing is NOT such a
 * failure — it is reported through `locked` and the ring still works. */
int adsc_wfifo_start(struct adsc_wfifo *f, size_t bytes, uint32_t sectors_per_slot,
                     int fd, const struct adsc_wfifo_seg *seg, unsigned nseg,
                     int byteswap);

/* Block until the ring is FULL (or the source ran out, or the reader failed).
 *
 * NOT an optimisation. Without it the first pop finds an empty ring and is
 * counted as a starvation — and under the underrun policy, on a drive with no
 * failover, that single false positive stops EVERY burn at sector 0. Measured
 * on the PX-716A: "FIFO empty at sector 0", low-water 1 of 111 slots, on a
 * burn where the host was so far ahead the producer waited 1766 times.
 *
 * It is also what the buffer is FOR: starting a burn with a full ring means
 * the protection is there from the first sector rather than arriving some
 * seconds in. cdrecord does the same, and for the same reason. */
int adsc_wfifo_prefill(struct adsc_wfifo *f, unsigned *slots_out);

/* Take the next chunk. Returns the number of sectors, 0 at end of stream, or
 * negative on a producer error.
 *
 * `*was_empty` is set when the consumer had to WAIT — i.e. the ring was dry and
 * the host had fallen behind the drive. That is the signal the burn engine's
 * underrun policy branches on, and it is reported per-call rather than only in
 * the totals because the FIRST occurrence is the one that decides. */
int adsc_wfifo_pop(struct adsc_wfifo *f, const uint8_t **data, int *was_empty);

/* Release the slot the last pop returned. Separate from pop so the consumer
 * can hand the ring's own memory straight to WRITE(10) — no copy on the burn
 * path at all. */
void adsc_wfifo_release(struct adsc_wfifo *f);

/* Stop and free. `discard` abandons what is queued (cancellation). Returns the
 * producer's first error, or 0. Safe on a zeroed struct never started. */
int adsc_wfifo_stop(struct adsc_wfifo *f, int discard);

/* Bytes for `seconds` of audio at `x` times CD-DA rate, clamped to `cap`.
 * Exposed for the CLI so the flag parser and the engine agree by construction
 * rather than by two implementations of one rule. */
size_t adsc_wfifo_bytes_for(double seconds, unsigned speed_x, size_t cap);

#endif /* ACCUDISC_WRITE_FIFO_H */
