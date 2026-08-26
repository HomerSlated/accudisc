/* The commanded-read engine: chunked READ CD with C2/subchannel companions,
 * per-sector retry narrowing with cache-defeat, zero-fill alignment, the
 * frame-accurate status map, and the caller-selected accuracy strategies —
 * C2-guided rescue, multi-pass consensus verification, boundary overlap
 * checking, and a descending speed ladder for problem-sector rereads.
 *
 * Invariants carried over from c2read:
 *  - AUDIO/C2/SUB for a sector always come from one READ CD transfer; when
 *    a rescue or consensus read is accepted, the WHOLE sector is replaced.
 *  - A hard-unreadable sector is zero-filled (PCM 0, C2 all-ones, SUB 0) so
 *    the delivered streams never desync from the sector count.
 *  - Synthetic all-ones C2 from zero-fill is excluded from the C2 stats.
 *
 * Trust model (measured by the c2bench confusion matrix): a fired C2 flag
 * reliably marks a bad byte, a clear flag is only a claim — hence verify
 * passes compare actual audio bytes between independent reads rather than
 * trusting "no C2" alone, and consensus demands two byte-identical reads. */

#include <stdlib.h>
#include <string.h>

#include "../mmc/mmc.h"
#include "engine.h"

/* Keep one READ CD transfer under 64 KiB (sg one-shot buffer comfort zone). */
#define ADSC_MAX_XFER 65535
#define ADSC_DEFAULT_RETRIES 2
/* Distance of the cache-defeat read: far enough to force a real seek. */
#define ADSC_FLUSH_DISTANCE 5000
/* Audio-only sector_len >= 2352 bounds the chunk at 27; round up. */
#define ADSC_CHUNK_MAX 32
#define ADSC_OVERLAP_MAX 8
/* Sectors either side of a Q-position mismatch to treat as suspect too. */
#define ADSC_QPOS_MARGIN 4u
#define ADSC_SPAN_MAX (ADSC_CHUNK_MAX + ADSC_OVERLAP_MAX)
/* Independent samples kept during consensus (the two passes + extras). */
#define ADSC_SAMPLES_MAX 6

static uint8_t sev_log2(uint32_t v)
{
    unsigned sev = 0;

    while (v) {
        sev++;
        v >>= 1;
    }
    return (uint8_t)(sev > 15 ? 15 : sev);
}

uint8_t adsc_map_c2_byte(uint32_t c2_bits)
{
    return (uint8_t)(ACCUDISC_MAP_C2 | (sev_log2(c2_bits) << 4));
}

uint8_t adsc_map_recovered_byte(unsigned attempts)
{
    if (attempts > 15)
        attempts = 15;
    return (uint8_t)(ACCUDISC_MAP_RECOVERED | (attempts << 4));
}

uint8_t adsc_map_suspect_byte(uint32_t diff_bytes)
{
    return (uint8_t)(ACCUDISC_MAP_SUSPECT | (sev_log2(diff_bytes) << 4));
}

uint8_t adsc_subq_byte(const accudisc_q *q, uint32_t expect_lba)
{
    /* crc_ok BEFORE adr, and the order is the whole content of this function.
     * accudisc_q_parse fills adr from q[0] whether or not the CRC verified —
     * it must, that byte is the frame-type header and there is nothing else to
     * read it from — so a corrupt frame routinely presents adr 2 or 3. Ask adr
     * first and such a frame is classified NO_POSITION, which every consumer of
     * this lane reads as HEALTHY. That is inverted polarity on exactly the
     * frames the lane exists to draw, and nothing downstream could catch it:
     * the byte is well-formed and names a state that genuinely occurs.
     *
     * No severity nibble: one CRC-16 over one 12-byte frame either verified or
     * did not, so there is no honest gradient to report. */
    if (!q->crc_ok)
        return ACCUDISC_SUBQ_BAD;
    if (q->adr != ACCUDISC_Q_POSITION)
        return ACCUDISC_SUBQ_NO_POSITION;
    /* The frame verified and it names a position. Whose? A drive that lost
     * lock and re-acquired elsewhere reports the place it actually reached,
     * with a valid CRC — so this frame is well-formed, self-consistent, and
     * about a different sector. Before this test the lane called that OK. */
    if (adsc_q_position_lba(q) != (int32_t)expect_lba)
        return ACCUDISC_SUBQ_MISPOSITION;
    return ACCUDISC_SUBQ_OK;
}

int32_t adsc_q_position_lba(const accudisc_q *q)
{
    return accudisc_msf_to_lba(q->abs_m, q->abs_s, q->abs_f);
}

uint32_t adsc_audio_diff(const uint8_t *a, const uint8_t *b)
{
    /* Fast path for the common case: two good reads that agree. memcmp is
     * vectorised and settles equality far faster than a scalar byte scan. Only
     * on a genuine mismatch do we pay the per-byte count — whose result is used
     * solely to size the suspect-byte severity, i.e. only on the rare path.
     * Bit-exact with the plain loop: both return 0 iff the buffers are equal. */
    if (memcmp(a, b, ACCUDISC_BYTES_AUDIO) == 0)
        return 0;

    uint32_t n = 0;
    for (uint32_t i = 0; i < ACCUDISC_BYTES_AUDIO; i++)
        n += a[i] != b[i];
    return n;
}

/* Slip detector for the error class C2 is structurally blind to: the drive
 * loses servo tracking and streams coherent audio from the WRONG position —
 * CIRC decodes it perfectly, no flag fires (see RECOVERY_STRATEGY.md, the
 * 8,852-sample observation). Two reads related by a pure shift are such a
 * slip; two reads with in-place differences are data instability. */
#define ADSC_SHIFT_MAX 588   /* samples: half a sector */
#define ADSC_ANCHOR_BYTES 64

int adsc_shift_find(const uint8_t *a, const uint8_t *b,
                    int32_t *shift_samples)
{
    const int32_t center = ACCUDISC_BYTES_AUDIO / 2;
    const uint8_t *anchor = a + center;

    /* A silent/constant anchor matches anywhere — no signal, no verdict. */
    int has_signal = 0;
    for (int32_t i = 1; i < ADSC_ANCHOR_BYTES; i++)
        has_signal |= anchor[i] != anchor[0];
    if (!has_signal)
        return 0;

    for (int32_t d = -ADSC_SHIFT_MAX; d <= ADSC_SHIFT_MAX; d++) {
        int32_t off = center + d * 4;

        if (d == 0 || off < 0 ||
            off + ADSC_ANCHOR_BYTES > (int32_t)ACCUDISC_BYTES_AUDIO)
            continue;
        if (memcmp(anchor, b + off, ADSC_ANCHOR_BYTES) != 0)
            continue;
        /* Anchor hit: the whole overlap must agree at this shift. */
        int32_t bd = d * 4;
        const uint8_t *pa = bd >= 0 ? a : a - bd;
        const uint8_t *pb = bd >= 0 ? b + bd : b;
        uint32_t len = ACCUDISC_BYTES_AUDIO - (uint32_t)(bd >= 0 ? bd : -bd);

        if (memcmp(pa, pb, len) == 0) {
            *shift_samples = d;
            return 1;
        }
    }
    return 0;
}

static void map_store(uint8_t *map, uint32_t idx, uint8_t v)
{
    if (map)
        __atomic_store_n(&map[idx], v, __ATOMIC_RELAXED);
}

static uint32_t popcount_buf(const uint8_t *p, uint32_t n)
{
    uint32_t bits = 0;

    for (uint32_t i = 0; i < n; i++)
        bits += (uint32_t)__builtin_popcount(p[i]);
    return bits;
}

/* ---- engine state --------------------------------------------------------- */

struct rd {
    struct accudisc_device *dev;
    const accudisc_read_req *req;
    unsigned sector_type;
    uint32_t sector_len, audio_len, c2_len, sub_len;
    unsigned retries;
    int speed_dirty;  /* a ladder speed is set; restore before streaming */
    int cur_speed;    /* last CDROM_SELECT_SPEED issued; -1 = unknown */
    uint8_t *flush;   /* cache-defeat throwaway, one audio sector */
    uint8_t *scratch; /* one sector, rescue/consensus candidate */
    uint8_t *samples; /* ADSC_SAMPLES_MAX sectors, consensus memory */
    accudisc_read_stats st;
};

static void count_sense(struct rd *r)
{
    if (r->dev->last_sense.valid && r->dev->last_sense.key == 0x3)
        r->st.sense_medium++;
    else if (r->dev->last_sense.valid && r->dev->last_sense.key == 0x4)
        r->st.sense_hardware++;
    else
        r->st.sense_other++;
}

/* Speed changes make real drives recalibrate (seconds each) — never issue
 * a no-op change. */
static void set_speed_once(struct rd *r, int speed)
{
    if (speed == r->cur_speed)
        return;
    accudisc_set_speed(r->dev, (unsigned)speed);
    r->cur_speed = speed;
}

/* Rescue/consensus attempt n runs at ladder[min(n-1, len-1)]. */
static void ladder_speed(struct rd *r, unsigned attempt)
{
    const accudisc_read_req *req = r->req;
    unsigned idx;

    if (!req->ladder_len || !req->speed_ladder)
        return;
    idx = attempt - 1;
    if (idx >= req->ladder_len)
        idx = (unsigned)req->ladder_len - 1;
    set_speed_once(r, req->speed_ladder[idx]);
    r->speed_dirty = 1;
}

/* Back to the pass speed (or the drive's own management) for streaming. */
static void ladder_restore(struct rd *r)
{
    if (!r->speed_dirty)
        return;
    set_speed_once(r, r->req->speed_x); /* 0 = drive default */
    r->speed_dirty = 0;
}

/* A back-to-back reread often just replays the drive cache; read one sector
 * far away first so the next read is a real disc access (readcd pattern). */
static void cache_defeat(struct rd *r, uint32_t cur)
{
    uint32_t away = cur >= r->req->lba + ADSC_FLUSH_DISTANCE
                        ? cur - ADSC_FLUSH_DISTANCE
                        : cur + ADSC_FLUSH_DISTANCE;

    adsc_mmc_read_cd(r->dev, away, 1, r->sector_type, ACCUDISC_C2_NONE,
                     ACCUDISC_SUB_NONE, r->flush, ACCUDISC_BYTES_AUDIO);
}

static int read_sector(struct rd *r, uint32_t lba, uint8_t *dst)
{
    return adsc_mmc_read_cd(r->dev, lba, 1, r->sector_type, r->req->c2,
                            r->req->sub, dst, r->sector_len);
}

/* Is the last failure a statement about the REQUEST rather than about this
 * copy of the disc? ILLEGAL REQUEST / ILLEGAL MODE FOR THIS TRACK (ASC 0x64)
 * is the drive saying the sector is not the type we asked for — a data track
 * read as CD-DA. That answer is identical on every attempt, so retrying it
 * buys nothing and costs a cache-defeat seek per sector: the 4,129-sector data
 * track that prompted this guard turned a read into an hours-long seek storm
 * with no possible result. Give up on the sector immediately. */
static int sense_is_categorical(struct rd *r)
{
    return r->dev->last_sense.valid && r->dev->last_sense.key == 0x5 &&
           r->dev->last_sense.asc == 0x64;
}

/* One span of total sectors with per-sector fallback. Sectors below
 * primary_n are deliverable: on unrecoverable failure they are zero-filled
 * and accounted as hard errors. Sectors at/above primary_n (overlap
 * extension, or all of a verify pass with primary_n 0) are only marked in
 * hard[] — their failure is a missing signal, not a hard error. */
static void read_span(struct rd *r, uint32_t lba, uint32_t total,
                      uint8_t *buf, uint8_t *hard, uint32_t primary_n)
{
    memset(hard, 0, total);
    if (adsc_mmc_read_cd(r->dev, lba, total, r->sector_type, r->req->c2,
                         r->req->sub, buf, r->sector_len) == ACCUDISC_OK)
        return;

    for (uint32_t s = 0; s < total; s++) {
        uint8_t *sec = buf + (size_t)s * r->sector_len;
        uint32_t cur = lba + s;
        int src = -1;

        for (unsigned attempt = 0; attempt < r->retries; attempt++) {
            if (attempt > 0)
                cache_defeat(r, cur);
            src = read_sector(r, cur, sec);
            if (src == ACCUDISC_OK)
                break;
            if (sense_is_categorical(r))
                break; /* the answer will not change; stop burning seeks */
        }
        if (src == ACCUDISC_OK)
            continue;
        hard[s] = 1;
        if (s < primary_n) {
            memset(sec, 0, r->audio_len);
            memset(sec + r->audio_len, 0xff, r->c2_len);
            if (r->sub_len)
                memset(sec + r->audio_len + r->c2_len, 0, r->sub_len);
            r->st.hard_errors++;
            count_sense(r);
        }
    }
}

/* Hunt for a C2-clean copy of a flagged sector; the read with the fewest
 * fired bits wins and replaces the sector wholesale. */
static void c2_rescue(struct rd *r, uint32_t lba, uint8_t *sec,
                      uint32_t *bits, unsigned *attempts)
{
    uint32_t best = *bits;

    *attempts = 0;
    for (unsigned a = 1; a <= r->req->c2_retries && best > 0; a++) {
        ladder_speed(r, a);
        cache_defeat(r, lba);
        if (read_sector(r, lba, r->scratch) != ACCUDISC_OK)
            continue;
        r->st.rereads++;
        *attempts = a;
        uint32_t b = popcount_buf(r->scratch + r->audio_len, r->c2_len);
        if (b < best) {
            memcpy(sec, r->scratch, r->sector_len);
            best = b;
        }
    }
    *bits = best;
}

/* Two reads disagreed on this sector. Collect further independent reads
 * until any two audio payloads (among everything seen) match byte-for-byte;
 * the agreeing read replaces the sector. 1 = recovered, 0 = suspect. */
/* Rescue a sector whose Q reported the drive was somewhere else.
 *
 * DELIBERATELY NOT consensus(). consensus() seeds its first sample with the
 * copy already in hand and accepts any reread that MATCHES it — which for this
 * fault is precisely the evidence that must not count. The slip REPRODUCES:
 * the same site was measured returning the same displaced data across five
 * independent whole-disc passes. Feed the bad copy in as a vote and a faithful
 * reproduction "confirms" it, the sector is relabelled RECOVERED, and the
 * check that found the corruption launders it. That is worse than not
 * checking at all.
 *
 * So the bad copy is never a vote. A replacement is accepted only when its OWN
 * Q frame agrees with the LBA that was asked for — validated by the same
 * independent signal that detected the fault, rather than by agreement with
 * the thing it is replacing. A reread whose Q is unreadable is not usable
 * either: with no claim there is nothing to check, and silence is not consent. */
static int qpos_rescue(struct rd *r, uint32_t lba, uint8_t *sec,
                       unsigned *attempts)
{
    for (unsigned a = 1; a <= ADSC_SAMPLES_MAX - 2; a++) {
        uint8_t qf[12];
        accudisc_q qd;

        ladder_speed(r, a);
        cache_defeat(r, lba);
        if (read_sector(r, lba, r->scratch) != ACCUDISC_OK)
            continue;
        r->st.rereads++;
        *attempts = a;
        accudisc_sub_extract_q(r->scratch + r->audio_len + r->c2_len, qf);
        accudisc_q_parse(qf, &qd);
        if (!qd.crc_ok || qd.adr != ACCUDISC_Q_POSITION)
            continue;
        if (adsc_q_position_lba(&qd) != (int32_t)lba)
            continue; /* still elsewhere */
        memcpy(sec, r->scratch, r->sector_len);
        return 1;
    }
    return 0;
}

static int consensus(struct rd *r, uint32_t lba, uint8_t *sec,
                     const uint8_t *alt, unsigned *attempts)
{
    unsigned count = 0;

    memcpy(r->samples, sec, r->sector_len);
    count = 1;
    if (alt) {
        memcpy(r->samples + r->sector_len, alt, r->sector_len);
        count = 2;
    }

    for (unsigned a = 1; a <= ADSC_SAMPLES_MAX - 2; a++) {
        ladder_speed(r, a);
        cache_defeat(r, lba);
        if (read_sector(r, lba, r->scratch) != ACCUDISC_OK)
            continue;
        r->st.rereads++;
        *attempts = a;
        for (unsigned i = 0; i < count; i++) {
            if (adsc_audio_diff(r->scratch,
                                r->samples + (size_t)i * r->sector_len)
                == 0) {
                memcpy(sec, r->scratch, r->sector_len);
                return 1;
            }
        }
        if (count < ADSC_SAMPLES_MAX) {
            memcpy(r->samples + (size_t)count * r->sector_len, r->scratch,
                   r->sector_len);
            count++;
        }
    }
    return 0;
}

int accudisc_read_cdda(accudisc_device *dev, const accudisc_read_req *req,
                       accudisc_sink_fn sink, void *user,
                       accudisc_read_stats *stats)
{
    struct rd r;
    accudisc_read_req local;
    size_t stats_bytes = 0;

    memset(&r, 0, sizeof(r));
    r.st.first_flagged_lba = -1;
    r.st.last_flagged_lba = -1;

    if (!req)
        return ACCUDISC_ERR_INVAL;

    /* Size negotiation before anything else touches the structs, and before the
     * device check, so a binding with a stale layout is diagnosed as ERR_ABI
     * ("rebuild") rather than as ERR_INVAL ("fix your arguments") — and so the
     * whole path is reachable in tests/test_abi.c without a drive. `req` is
     * only guaranteed to be req->size bytes long, so everything downstream
     * reads the local copy, never the caller's. */
    int abi = adsc_abi_import(&local, sizeof(local), req, req->size);
    if (abi != ACCUDISC_OK)
        return abi;
    if (stats) {
        abi = adsc_abi_export(stats->size, sizeof(*stats), &stats_bytes);
        if (abi != ACCUDISC_OK)
            return abi;
    }
    req = &local;

    if (!dev || req->count == 0)
        return ACCUDISC_ERR_INVAL;
    if (req->c2 > ACCUDISC_C2_PTRS_BEB || req->sub > ACCUDISC_SUB_Q)
        return ACCUDISC_ERR_INVAL;
    /* The Q health lane needs the raw P-W frame to run its own CRC over. SUB_Q
     * is drive-formatted and already CRC-gated inside the drive, so we would be
     * reporting the drive's opinion as our measurement; SUB_NONE has nothing to
     * measure at all. Refuse both rather than fill the map with a single state —
     * a lane that is uniform because nothing was measured is indistinguishable,
     * to a renderer, from a lane that is uniform because the disc is perfect.
     *
     * After adsc_abi_import, so a 0.4 caller (whose struct ends before this
     * field) zero-extends to NULL and cannot trip a check for a feature it has
     * never heard of. */
    if (req->subq_map && req->sub != ACCUDISC_SUB_RAW)
        return ACCUDISC_ERR_INVAL;

    /* REMOVED IN 0.6.0: a refusal to capture subchannel while the vendor
     * read-speed uncap was authoritatively on, overridable via
     * req->allow_unsafe. Keith's ruling, 2026-08-09.
     *
     * The guard rested on a premise that is false. It assumed the uncap raises
     * the CD-DA read rate, and it cannot: the drive's governor caps CD-DA at
     * 40x whatever the uncap is set to, and mode page 2A reports the REQUESTED
     * figure rather than the governed throughput — so the 48x that appeared to
     * make the combination dangerous was a number, not a speed. The Q damage
     * that motivated the guard was measured at 40x, on both sides of the A/B.
     * A guard against a state the hardware cannot enter costs its callers a
     * refusal and buys nothing.
     *
     * The queryable state stays: accudisc_speed_uncap_probe/set/get/push/pop,
     * the `speed-uncap` subcommand and `--uncap` are untouched. Reporting a
     * drive's configuration is not the same as refusing to work in it. */

    r.dev = dev;
    r.req = req;
    r.sector_type = req->any_type ? ADSC_SECTOR_ANY : ADSC_SECTOR_CDDA;
    r.sector_len = adsc_read_cd_sector_len(req->c2, req->sub);
    r.audio_len = ACCUDISC_BYTES_AUDIO;
    r.sub_len = req->sub == ACCUDISC_SUB_RAW  ? ACCUDISC_BYTES_SUB_RAW
                : req->sub == ACCUDISC_SUB_Q  ? ACCUDISC_BYTES_SUB_Q
                                              : 0;
    r.c2_len = r.sector_len - r.audio_len - r.sub_len;
    r.retries = req->retries ? req->retries : ADSC_DEFAULT_RETRIES;
    r.cur_speed = -1;

    uint32_t overlap = req->overlap_sectors;
    if (overlap > ADSC_OVERLAP_MAX)
        overlap = ADSC_OVERLAP_MAX;

    uint32_t max_chunk = ADSC_MAX_XFER / r.sector_len;
    if (max_chunk > ADSC_CHUNK_MAX)
        max_chunk = ADSC_CHUNK_MAX;
    if (overlap >= max_chunk)
        overlap = max_chunk - 1;
    uint32_t chunk = req->chunk_sectors ? req->chunk_sectors
                                        : max_chunk - overlap;
    if (chunk > max_chunk - overlap)
        chunk = max_chunk - overlap;

    unsigned passes = req->verify_passes >= 2 ? req->verify_passes : 1;

    /* Apply the pass speed and record what the drive actually adopted, so a
     * quantized request (16x -> 8x on a PX-716A) is observable in the stats
     * rather than only in CLI prose. See accudisc_read_stats' four-state note.
     *
     * The read-back is gated on the SET SUCCEEDING. Page 2A always reports some
     * current speed, and after a failed set that value describes the drive's
     * prior state — exporting it as the honoured speed would report a
     * quantization that never happened. Leaving honoured at 0 says "asked, no
     * answer", which is the truth in that case.
     *
     * Best-effort throughout: a drive that will not report page 2A leaves
     * honoured at 0 and the read proceeds, exactly as before this field
     * existed. Nothing here can fail the read. */
    if (req->speed_x) {
        r.st.speed_requested_x = req->speed_x;
        if (accudisc_set_speed(dev, req->speed_x) == ACCUDISC_OK) {
            unsigned cur_kbps = 0;
            if (accudisc_get_speed(dev, NULL, &cur_kbps) == ACCUDISC_OK)
                r.st.speed_honoured_x = (uint16_t)(cur_kbps / 176);
        }
    }

    uint8_t *buf = malloc((size_t)(chunk + overlap) * r.sector_len);
    uint8_t *buf2 = passes > 1 ? malloc((size_t)chunk * r.sector_len) : NULL;
    uint8_t *prev_ext =
        overlap ? malloc((size_t)overlap * r.sector_len) : NULL;
    r.flush = malloc(ACCUDISC_BYTES_AUDIO);
    r.scratch = malloc(r.sector_len);
    r.samples = malloc((size_t)ADSC_SAMPLES_MAX * r.sector_len);
    int rc = ACCUDISC_OK;

    uint8_t prev_ext_hard[ADSC_OVERLAP_MAX];
    uint32_t prev_ext_n = 0;

    if (!buf || !r.flush || !r.scratch || !r.samples ||
        (passes > 1 && !buf2) || (overlap && !prev_ext)) {
        rc = ACCUDISC_ERR_NOMEM;
        goto out;
    }

    uint32_t lba = req->lba;
    uint32_t remaining = req->count;
    while (remaining > 0) {
        uint32_t n = remaining < chunk ? remaining : chunk;
        /* Extension only exists when another chunk will follow it. */
        uint32_t ext = (overlap && remaining > n)
                           ? (remaining - n < overlap ? remaining - n
                                                      : overlap)
                           : 0;
        uint8_t hard[ADSC_SPAN_MAX], hard2[ADSC_CHUNK_MAX];
        uint8_t recov[ADSC_CHUNK_MAX] = {0}, susp[ADSC_CHUNK_MAX] = {0};
        unsigned att[ADSC_CHUNK_MAX] = {0};
        uint32_t bits[ADSC_CHUNK_MAX] = {0}, diffb[ADSC_CHUNK_MAX] = {0};

        if (req->cancel && *req->cancel) {
            rc = ACCUDISC_ERR_CANCELLED;
            goto out;
        }

        ladder_restore(&r);
        read_span(&r, lba, n + ext, buf, hard, n);

        /* Q-POSITION CHECK — the drive's own account of where it was.
         *
         * Every other check in this loop compares two of the drive's answers
         * to each other. This one compares its answer to the QUESTION, which
         * is why it survives a fault that reproduces: a drive that lost lock
         * and re-acquired 2048 sectors early returns valid audio AND a
         * CRC-valid Q frame naming the place it actually reached. C2 is
         * silent, repeated reads agree with each other, and the seam check
         * looks at seams while the fault sits mid-transfer. Measured on a
         * PX-716A: 17/17 caught, zero false positives over 1.4 M sectors.
         *
         * Needs raw P-W. With no subchannel there is no claim to contradict,
         * and this whole block is skipped. */
        if (r.sub_len == ACCUDISC_BYTES_SUB_RAW) {
            uint8_t qbad[ADSC_CHUNK_MAX] = {0}, qsus[ADSC_CHUNK_MAX] = {0};

            for (uint32_t s = 0; s < n; s++) {
                const uint8_t *sub;
                uint8_t qf[12];
                accudisc_q qd;

                if (hard[s])
                    continue; /* zero-filled: its Q is fabricated, not read */
                sub = buf + (size_t)s * r.sector_len + r.audio_len + r.c2_len;
                accudisc_sub_extract_q(sub, qf);
                accudisc_q_parse(qf, &qd);
                if (qd.crc_ok && qd.adr == ACCUDISC_Q_POSITION &&
                    adsc_q_position_lba(&qd) != (int32_t)(lba + s)) {
                    qbad[s] = 1;
                    /* Counted here, on the sectors that actually disagreed —
                     * NOT on the widened set below. The margin is a
                     * precaution; reporting it would inflate the number past
                     * what was measured. */
                    r.st.subq_misposition++;
                }
            }

            /* Widen by a margin. The LEADING EDGE of a slip carries correct Q
             * with already-wrong audio — the two channels are offset in time,
             * so the audio goes bad a sector or two before Q admits it. The
             * worst case measured on this drive was 2 sectors; the margin is
             * that, doubled, because it is cheap (the widened sectors are
             * merely rechecked, not discarded) and being one sector short
             * leaks exactly the corruption this exists to catch. Computed
             * into a second array so a widened sector cannot widen further. */
            for (uint32_t s = 0; s < n; s++) {
                if (!qbad[s])
                    continue;
                uint32_t lo = s > ADSC_QPOS_MARGIN ? s - ADSC_QPOS_MARGIN : 0;
                uint32_t hi = s + ADSC_QPOS_MARGIN;

                if (hi >= n)
                    hi = n - 1;
                for (uint32_t k = lo; k <= hi; k++)
                    qsus[k] = 1;
            }

            for (uint32_t s = 0; s < n; s++) {
                unsigned used = 0;

                if (!qsus[s] || hard[s] || susp[s])
                    continue;
                if (qpos_rescue(&r, lba + s, buf + (size_t)s * r.sector_len,
                                &used)) {
                    recov[s] = 1;
                    att[s] += used;
                } else {
                    susp[s] = 1;
                }
            }
        }

        /* Boundary overlap check: the previous chunk read past its seam;
         * those extension sectors must byte-match this chunk's head. A
         * mismatch means one of the two reads slipped — consensus decides. */
        for (uint32_t s = 0; s < prev_ext_n && s < n; s++) {
            if (prev_ext_hard[s] || hard[s] || susp[s])
                continue;
            uint8_t *sec = buf + (size_t)s * r.sector_len;
            const uint8_t *alt = prev_ext + (size_t)s * r.sector_len;
            uint32_t diff = adsc_audio_diff(sec, alt);

            if (diff == 0)
                continue;
            int32_t sh;
            if (adsc_shift_find(sec, alt, &sh))
                r.st.slips++;
            diffb[s] = diff;
            unsigned used = 0;
            if (consensus(&r, lba + s, sec, alt, &used)) {
                recov[s] = 1;
                att[s] += used;
            } else {
                susp[s] = 1;
            }
        }

        for (uint32_t s = 0; s < n; s++)
            if (!hard[s] && r.c2_len)
                bits[s] = popcount_buf(buf + (size_t)s * r.sector_len +
                                       r.audio_len, r.c2_len);

        /* C2-guided rescue: hunt a clean copy of every flagged sector. */
        if (req->c2_retries && r.c2_len) {
            for (uint32_t s = 0; s < n; s++) {
                if (hard[s] || bits[s] == 0)
                    continue;
                uint32_t before = bits[s];
                unsigned used = 0;
                c2_rescue(&r, lba + s, buf + (size_t)s * r.sector_len,
                          &bits[s], &used);
                if (bits[s] == 0 && before > 0) {
                    recov[s] = 1;
                    att[s] += used;
                }
            }
        }

        /* Verify passes: independent rereads must agree byte-for-byte.
         * Passes run at streaming speed — real drives recalibrate on every
         * speed change, so chunk-granular speed switching thrashes. Speed
         * diversity against persistent same-speed misreads (RECOVERY_
         * STRATEGY R6) comes from the consensus/rescue rereads, which run
         * the ladder per sector; whole-range speed-diverse sweeps are the
         * caller's layer (multiple reads at different speed_x). Pick ladder
         * rungs that differ from speed_x so the deciding votes really are
         * speed-diverse. */
        for (unsigned pass = 2; pass <= passes; pass++) {
            ladder_restore(&r);
            cache_defeat(&r, lba);
            read_span(&r, lba, n, buf2, hard2, 0);
            for (uint32_t s = 0; s < n; s++) {
                if (hard[s] || susp[s])
                    continue;
                uint8_t *sec = buf + (size_t)s * r.sector_len;
                const uint8_t *alt = buf2 + (size_t)s * r.sector_len;
                uint32_t diff =
                    hard2[s] ? 0 : adsc_audio_diff(sec, alt);

                if (!hard2[s] && diff == 0)
                    continue; /* confirmed */
                if (!hard2[s]) {
                    int32_t sh;
                    if (adsc_shift_find(sec, alt, &sh))
                        r.st.slips++;
                }
                diffb[s] = diff;
                unsigned used = 0;
                if (consensus(&r, lba + s, sec, hard2[s] ? NULL : alt,
                              &used)) {
                    recov[s] = 1;
                    att[s] += used;
                    if (r.c2_len)
                        bits[s] = popcount_buf(sec + r.audio_len, r.c2_len);
                } else {
                    susp[s] = 1;
                }
            }
        }

        /* Classify, account, publish. Priority: hard > suspect > recovered
         * > C2 > ok. */
        for (uint32_t s = 0; s < n; s++) {
            uint32_t cur = lba + s;
            uint32_t idx = cur - req->lba;

            if (hard[s]) {
                map_store(req->status_map, idx, ACCUDISC_MAP_HARD);
                /* No frame was delivered — the sector is zero-filled. Saying so
                 * is the whole reason this lane belongs in the engine: a
                 * zero-filled Q frame FAILS CRC-16, so anyone recomputing this
                 * downstream records fabricated Q damage here. */
                map_store(req->subq_map, idx, ACCUDISC_SUBQ_NO_AUDIO);
                continue;
            }
            r.st.sectors_read++;
            r.st.c2_bits += bits[s];

            /* Q-subchannel CRC health of the delivered sector. Raw P-W only;
             * the drive-formatted Q path (SUB_Q) is already CRC-gated in the
             * drive. This is independent of the C2 audio stats — a clean-audio
             * sector can still carry a corrupt Q frame (lost pregap/index). */
            if (r.sub_len == ACCUDISC_BYTES_SUB_RAW) {
                const uint8_t *sub =
                    buf + (size_t)s * r.sector_len + r.audio_len + r.c2_len;
                uint8_t q[12];
                accudisc_q qd;

                accudisc_sub_extract_q(sub, q);
                accudisc_q_parse(q, &qd);
                r.st.subq_total++;
                if (qd.crc_ok)
                    r.st.subq_ok++;

                map_store(req->subq_map, idx, adsc_subq_byte(&qd, cur));
            }

            if (bits[s]) {
                r.st.sectors_flagged++;
                if (bits[s] > r.st.max_bits_sector)
                    r.st.max_bits_sector = bits[s];
                if (r.st.first_flagged_lba < 0)
                    r.st.first_flagged_lba = cur;
                r.st.last_flagged_lba = cur;
            }
            if (susp[s]) {
                r.st.sectors_suspect++;
                map_store(req->status_map, idx,
                          adsc_map_suspect_byte(diffb[s]));
            } else if (recov[s]) {
                r.st.sectors_recovered++;
                map_store(req->status_map, idx,
                          adsc_map_recovered_byte(att[s]));
            } else if (bits[s]) {
                map_store(req->status_map, idx, adsc_map_c2_byte(bits[s]));
            } else {
                map_store(req->status_map, idx, ACCUDISC_MAP_OK);
            }
        }

        if (sink) {
            accudisc_chunk out = {
                .lba = lba,
                .nsec = n,
                .data = buf,
                .sector_len = r.sector_len,
                .audio_len = r.audio_len,
                .c2_len = r.c2_len,
                .sub_len = r.sub_len,
            };
            if (sink(user, &out) != 0) {
                rc = ACCUDISC_ERR_CANCELLED;
                goto out;
            }
        }

        /* Stash the extension as the seam sample for the next chunk. */
        if (ext) {
            memcpy(prev_ext, buf + (size_t)n * r.sector_len,
                   (size_t)ext * r.sector_len);
            memcpy(prev_ext_hard, hard + n, ext);
        }
        prev_ext_n = ext;

        lba += n;
        remaining -= n;
    }
    ladder_restore(&r);

out:
    free(buf);
    free(buf2);
    free(prev_ext);
    free(r.flush);
    free(r.scratch);
    free(r.samples);
    if (stats) {
        /* Only as far as the caller said their allocation reaches — a whole
         * struct assignment here would write sizeof(*stats) bytes into memory
         * that may be an older, shorter layout. stats_bytes was settled by
         * adsc_abi_export on the way in. */
        r.st.size = (uint32_t)stats_bytes;
        memcpy(stats, &r.st, stats_bytes);
    }
    return rc;
}
