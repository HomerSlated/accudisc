/* The commanded-read engine: chunked READ CD with C2/subchannel companions,
 * per-sector retry narrowing with cache-defeat, zero-fill alignment, and the
 * frame-accurate status map. Port of the c2read main loop with the policy
 * hooks (sink, map, cancel) the library contract adds.
 *
 * Invariants carried over from c2read:
 *  - AUDIO/C2/SUB for a sector always come from one READ CD transfer.
 *  - A hard-unreadable sector is zero-filled (PCM 0, C2 all-ones, SUB 0) so
 *    the delivered streams never desync from the sector count.
 *  - Synthetic all-ones C2 from zero-fill is excluded from the C2 stats, so
 *    stats reflect the drive, not our padding.
 */

#include <stdlib.h>
#include <string.h>

#include "../mmc/mmc.h"
#include "engine.h"

/* Keep one READ CD transfer under 64 KiB (sg one-shot buffer comfort zone). */
#define ADSC_MAX_XFER 65535
#define ADSC_DEFAULT_RETRIES 2
/* Distance of the cache-defeat read: far enough to force a real seek. */
#define ADSC_FLUSH_DISTANCE 5000

uint8_t adsc_map_c2_byte(uint32_t c2_bits)
{
    unsigned sev = 0;

    while (c2_bits) { /* log2 + 1, clamped to the nibble */
        sev++;
        c2_bits >>= 1;
    }
    if (sev > 15)
        sev = 15;
    return (uint8_t)(ACCUDISC_MAP_C2 | (sev << 4));
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

/* Classify a hard failure's sense (already decoded into the handle). */
static void count_sense(const struct accudisc_device *dev,
                        accudisc_read_stats *st)
{
    if (dev->last_sense.valid && dev->last_sense.key == 0x3)
        st->sense_medium++;
    else if (dev->last_sense.valid && dev->last_sense.key == 0x4)
        st->sense_hardware++;
    else
        st->sense_other++;
}

int accudisc_read_cdda(accudisc_device *dev, const accudisc_read_req *req,
                       accudisc_sink_fn sink, void *user,
                       accudisc_read_stats *stats)
{
    accudisc_read_stats st;

    memset(&st, 0, sizeof(st));
    st.first_flagged_lba = -1;
    st.last_flagged_lba = -1;

    if (!dev || !req || req->count == 0)
        return ACCUDISC_ERR_INVAL;
    if (req->c2 > ACCUDISC_C2_PTRS_BEB || req->sub > ACCUDISC_SUB_Q)
        return ACCUDISC_ERR_INVAL;

    unsigned sector_type = req->any_type ? ADSC_SECTOR_ANY : ADSC_SECTOR_CDDA;
    uint32_t sector_len = adsc_read_cd_sector_len(req->c2, req->sub);
    uint32_t audio_len = ACCUDISC_BYTES_AUDIO;
    uint32_t c2_len = sector_len - audio_len -
                      (req->sub == ACCUDISC_SUB_RAW ? ACCUDISC_BYTES_SUB_RAW
                       : req->sub == ACCUDISC_SUB_Q ? ACCUDISC_BYTES_SUB_Q
                                                    : 0);
    uint32_t sub_len = sector_len - audio_len - c2_len;

    uint32_t max_chunk = ADSC_MAX_XFER / sector_len;
    uint32_t chunk = req->chunk_sectors ? req->chunk_sectors : max_chunk;
    if (chunk > max_chunk)
        chunk = max_chunk;
    unsigned retries = req->retries ? req->retries : ADSC_DEFAULT_RETRIES;

    if (req->speed_x)
        accudisc_set_speed(dev, req->speed_x);

    uint8_t *buf = malloc((size_t)chunk * sector_len);
    uint8_t *flushbuf = malloc(ACCUDISC_BYTES_AUDIO); /* cache-defeat throwaway */
    uint8_t *hard = calloc(chunk, 1); /* per-sector zero-fill marks */
    int rc = ACCUDISC_OK;

    if (!buf || !flushbuf || !hard) {
        rc = ACCUDISC_ERR_NOMEM;
        goto out;
    }

    uint32_t lba = req->lba;
    uint32_t remaining = req->count;
    while (remaining > 0) {
        uint32_t n = remaining < chunk ? remaining : chunk;

        if (req->cancel && *req->cancel) {
            rc = ACCUDISC_ERR_CANCELLED;
            goto out;
        }

        memset(hard, 0, n);
        int crc = adsc_mmc_read_cd(dev, lba, n, sector_type, req->c2,
                                   req->sub, buf, sector_len);
        if (crc != ACCUDISC_OK) {
            /* Whole-chunk failure: the drive could not return these sectors
             * at all (distinct from a C2 flag, which returns data). Narrow to
             * per-sector reads; a sector that still fails after the retry
             * budget is zero-filled. */
            for (uint32_t s = 0; s < n; s++) {
                uint8_t *sec = buf + (size_t)s * sector_len;
                uint32_t cur = lba + s;
                int src = -1;

                for (unsigned attempt = 0; attempt < retries; attempt++) {
                    if (attempt > 0) {
                        /* A back-to-back retry often just replays the drive
                         * cache; read one sector far away first so the retry
                         * is a real disc access (readcd pattern). */
                        uint32_t away = cur >= req->lba + ADSC_FLUSH_DISTANCE
                                            ? cur - ADSC_FLUSH_DISTANCE
                                            : cur + ADSC_FLUSH_DISTANCE;
                        adsc_mmc_read_cd(dev, away, 1, sector_type,
                                         ACCUDISC_C2_NONE, ACCUDISC_SUB_NONE,
                                         flushbuf, ACCUDISC_BYTES_AUDIO);
                    }
                    src = adsc_mmc_read_cd(dev, cur, 1, sector_type, req->c2,
                                           req->sub, sec, sector_len);
                    if (src == ACCUDISC_OK)
                        break;
                }
                if (src != ACCUDISC_OK) {
                    memset(sec, 0, audio_len);
                    memset(sec + audio_len, 0xff, c2_len);
                    if (sub_len)
                        memset(sec + audio_len + c2_len, 0, sub_len);
                    hard[s] = 1;
                    st.hard_errors++;
                    count_sense(dev, &st);
                }
            }
        }

        for (uint32_t s = 0; s < n; s++) {
            const uint8_t *sec = buf + (size_t)s * sector_len;
            uint32_t cur = lba + s;
            uint32_t idx = cur - req->lba;

            if (hard[s]) {
                map_store(req->status_map, idx, ACCUDISC_MAP_HARD);
                continue; /* synthetic C2 stays out of the stats */
            }
            uint32_t bits = c2_len ? popcount_buf(sec + audio_len, c2_len) : 0;
            st.sectors_read++;
            st.c2_bits += bits;
            if (bits) {
                st.sectors_flagged++;
                if (bits > st.max_bits_sector)
                    st.max_bits_sector = bits;
                if (st.first_flagged_lba < 0)
                    st.first_flagged_lba = cur;
                st.last_flagged_lba = cur;
                map_store(req->status_map, idx, adsc_map_c2_byte(bits));
            } else {
                map_store(req->status_map, idx, ACCUDISC_MAP_OK);
            }
        }

        if (sink) {
            accudisc_chunk out = {
                .lba = lba,
                .nsec = n,
                .data = buf,
                .sector_len = sector_len,
                .audio_len = audio_len,
                .c2_len = c2_len,
                .sub_len = sub_len,
            };
            if (sink(user, &out) != 0) {
                rc = ACCUDISC_ERR_CANCELLED;
                goto out;
            }
        }

        lba += n;
        remaining -= n;
    }

out:
    free(buf);
    free(flushbuf);
    free(hard);
    if (stats)
        *stats = st;
    return rc;
}
