/* accudisc_compare_read (0.59.0): a read held against the caller's reference.
 *
 * The read is accudisc_read_cdda's, unchanged; the comparison sits in a sink
 * wrapped around the caller's. That is why the reference callback runs where
 * the sink runs (the AccuBuffer thread by default), the contract published in
 * AccuDisc correspondence 2026-11-25aa: one place reads, one place compares,
 * and the engine needs no second code path that could drift from the first.
 *
 * HARD comes from the status map, which publish() writes before the chunk is
 * handed on, so it is visible here. When the caller passes no status map, an
 * internal one stands in. */

#include <stdlib.h>
#include <string.h>

#include "../internal.h"
#include "engine.h"

struct cmp {
    accudisc_ref_fn ref;
    void *ref_user;
    uint8_t *agree_map;
    accudisc_sink_fn sink;
    void *user;
    const uint8_t *status;
    uint32_t lba0;
    uint8_t *refbuf, *absent;
    size_t cap; /* sectors refbuf and absent hold */
    int nomem;
    int bad_absent; /* the callback wrote a reserved absent[] value */
    accudisc_compare_stats cs;
};

static uint8_t classify(struct cmp *k, const accudisc_chunk *c, uint32_t s,
                        const uint8_t *y)
{
    uint32_t idx = c->lba + s - k->lba0;
    const uint8_t *x = c->data + (size_t)s * c->sector_len;
    uint8_t st = __atomic_load_n(&k->status[idx], __ATOMIC_RELAXED) & 0x0f;

    if (st == ACCUDISC_MAP_HARD) {
        k->cs.hard++;
        return ACCUDISC_AGREE_HARD;
    }
    if (k->absent[s]) {
        k->cs.ref_missing++;
        return ACCUDISC_AGREE_REF_MISSING;
    }
    if (memcmp(x, y, ACCUDISC_BYTES_AUDIO) == 0) {
        const uint8_t *c2 = x + c->audio_len;
        int flagged = 0;

        for (uint32_t i = 0; i < c->c2_len; i++)
            flagged |= c2[i] != 0;
        if (flagged) {
            k->cs.agree_c2++;
            return ACCUDISC_AGREE_C2;
        }
        k->cs.agree++;
        return ACCUDISC_AGREE;
    }
    /* x is this pass's copy, the one whose alignment is judged; the union of
     * shift_search, always (a classification, not a recovery decision) */
    int32_t sh;
    if (adsc_shift_find(x, y, &sh) ||
        accudisc_shift_test(x, y, ACCUDISC_BYTES_AUDIO, &sh) == 1) {
        uint32_t mag = (uint32_t)(sh < 0 ? -sh : sh);
        uint32_t frames = 0;

        k->cs.shifted++;
        if (sh % 6 == 0)
            frames = mag / 6 > 15 ? 15 : mag / 6;
        else
            k->cs.shifted_off_frame++;
        return (uint8_t)(ACCUDISC_AGREE_SHIFTED | frames << 4);
    }
    k->cs.differ++;
    return ACCUDISC_AGREE_DIFFER;
}

static int cmp_sink(void *u, const accudisc_chunk *c)
{
    struct cmp *k = u;

    if (c->nsec > k->cap) {
        free(k->refbuf);
        free(k->absent);
        k->refbuf = malloc((size_t)c->nsec * ACCUDISC_BYTES_AUDIO);
        k->absent = malloc(c->nsec);
        k->cap = c->nsec;
        if (!k->refbuf || !k->absent) {
            k->cap = 0;
            k->nomem = 1;
            return 1;
        }
    }
    memset(k->absent, 0, c->nsec);
    if (k->ref(k->ref_user, c->lba, c->nsec, k->refbuf, k->absent))
        return 1; /* the caller cancelled */
    for (uint32_t s = 0; s < c->nsec; s++)
        if (k->absent[s] > 1) { /* reserved: never read as "absent" */
            k->bad_absent = 1;
            return 1;
        }
    for (uint32_t s = 0; s < c->nsec; s++) {
        uint8_t v = classify(k, c, s,
                             k->refbuf + (size_t)s * ACCUDISC_BYTES_AUDIO);

        if (k->agree_map)
            __atomic_store_n(&k->agree_map[c->lba + s - k->lba0], v,
                             __ATOMIC_RELAXED);
    }
    return k->sink ? k->sink(k->user, c) : 0;
}

int accudisc_compare_read(accudisc_device *dev, const accudisc_read_req *req,
                          accudisc_ref_fn ref, void *ref_user,
                          uint8_t *agree_map, accudisc_sink_fn sink,
                          void *user, accudisc_read_stats *stats,
                          accudisc_compare_stats *cstats)
{
    accudisc_read_req rq;
    size_t cs_n = 0;
    uint8_t *own_map = NULL;
    int rc;

    if (!ref || !req)
        return ACCUDISC_ERR_INVAL;
    if (cstats) {
        rc = adsc_abi_export(cstats->size, sizeof(*cstats), &cs_n);
        if (rc != ACCUDISC_OK)
            return rc;
    }
    /* Import BEFORE copying: an older caller's struct is shorter than ours. */
    rc = adsc_abi_import(&rq, sizeof(rq), req, req->size);
    if (rc != ACCUDISC_OK)
        return rc;

    struct cmp k;
    memset(&k, 0, sizeof(k));
    k.ref = ref;
    k.ref_user = ref_user;
    k.agree_map = agree_map;
    k.sink = sink;
    k.user = user;
    k.lba0 = rq.lba;
    if (!rq.status_map && rq.count) {
        own_map = calloc(rq.count, 1);
        if (!own_map)
            return ACCUDISC_ERR_NOMEM;
        rq.status_map = own_map;
    }
    k.status = rq.status_map;

    rc = accudisc_read_cdda(dev, &rq, rq.count ? cmp_sink : sink,
                            rq.count ? (void *)&k : user, stats);
    if (k.nomem && rc == ACCUDISC_ERR_CANCELLED)
        rc = ACCUDISC_ERR_NOMEM;
    if (k.bad_absent && rc == ACCUDISC_ERR_CANCELLED)
        rc = ACCUDISC_ERR_INVAL;
    if (cstats) {
        k.cs.size = (uint32_t)cs_n;
        memcpy(cstats, &k.cs, cs_n);
    }
    free(k.refbuf);
    free(k.absent);
    free(own_map);
    return rc;
}
