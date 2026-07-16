/* Index / pregap map: decode a raw-subchannel scan into a per-track index map,
 * cross-referenced against the TOC's authoritative index-1 boundaries.
 *
 * The TOC hands us only index-1 (track start). Index 0 (pregap) and any
 * intra-track indices live solely in the Q subchannel, which has no CIRC — a
 * per-frame CRC-16 is its only integrity check. So every frame is CRC-gated
 * before it is trusted: damage on this disc does not merely lose Q, it injects
 * false Q (absurd MSF, phantom indices), and only the CRC separates the two.
 *
 * This pass is deliberately observational. At a boundary whose approach is
 * damaged it reports UNKNOWN rather than guessing a pregap into or out of
 * existence; reconstructing across the gap (abs-MSF continuity, rel countdown)
 * is a separate, model-based step. */

#include <string.h>

#include <accudisc/accudisc.h>

/* How far before a boundary to hunt for the track's index-0 frames. Generous:
 * conventional pregaps are <=150 frames (2 s), but we allow oddballs. */
#define PREGAP_SCAN_FRAMES 400
/* The tight approach window whose cleanliness decides NONE vs UNKNOWN. A
 * CRC-bad frame this close to the boundary, with no index-0 seen, means a short
 * pregap could be hidden behind the damage. */
#define CLEAN_WINDOW_FRAMES 150

static uint32_t rel_frames(const accudisc_q *q)
{
    return ((uint32_t)q->rel_m * 60 + q->rel_s) * 75 + q->rel_f;
}

uint32_t accudisc_index_map_decode(const uint8_t *raw, int32_t base_lba,
                                   uint32_t count, const accudisc_toc *toc,
                                   accudisc_index_map *out, uint32_t max_out)
{
    if (!raw || !toc || !out || !max_out)
        return 0;

    const int32_t scan_end = base_lba + (int32_t)count; /* exclusive */
    uint32_t written = 0;

    for (uint8_t ti = 0; ti < toc->track_count && written < max_out; ti++) {
        const accudisc_track *trk = &toc->tracks[ti];
        const int32_t L = (int32_t)trk->lba;             /* index-1 (TOC truth) */

        accudisc_index_map *m = &out[written++];
        memset(m, 0, sizeof(*m));
        m->track = trk->number;
        m->index1_lba = L;
        m->q_index1_lba = -1;
        m->index0_lba = -1;

        /* Coverage gate: we must at least hold the frame right before the
         * boundary, or we cannot say anything about a pregap here. */
        if (base_lba > L - 1 || scan_end <= L - 1) {
            m->pregap_state = ACCUDISC_PREGAP_NO_DATA;
            continue;
        }

        const int32_t scan_lo = L - PREGAP_SCAN_FRAMES;
        const int32_t clean_lo = L - CLEAN_WINDOW_FRAMES;
        int pregap_seen = 0;
        uint32_t max_rel = 0;

        /* Walk [scan_lo, L+1]: the +1 catches the index-1 frame at exactly L
         * for the Q-vs-TOC cross-check. */
        for (int32_t lba = scan_lo; lba <= L; lba++) {
            if (lba < base_lba || lba >= scan_end)
                continue;
            const uint8_t *sub = raw + (size_t)(lba - base_lba) * 96;
            uint8_t qb[12];
            accudisc_q q;
            accudisc_sub_extract_q(sub, qb);
            accudisc_q_parse(qb, &q);

            const int in_clean = (lba >= clean_lo && lba < L);
            if (!q.crc_ok) {
                if (in_clean)
                    m->crc_bad++;
                continue;
            }
            if (in_clean)
                m->crc_ok++;
            if (q.adr != ACCUDISC_Q_POSITION)
                continue;

            if (q.tno == m->track) {
                if (q.index > m->max_index)
                    m->max_index = q.index;
                if (q.index == 0 && lba < L) {
                    pregap_seen = 1;
                    uint32_t r = rel_frames(&q);
                    if (r > max_rel)
                        max_rel = r; /* transition frame carries the length */
                } else if (q.index == 1) {
                    if (m->q_index1_lba < 0 || lba < m->q_index1_lba)
                        m->q_index1_lba = lba;
                }
            }
        }

        if (pregap_seen && max_rel > 0) {
            m->pregap_state = ACCUDISC_PREGAP_PRESENT;
            m->pregap_frames = max_rel;
            m->index0_lba = L - (int32_t)max_rel;
            continue;
        }

        /* No index-0 seen: gapless or damage-hidden. A pregap abuts index-1
         * (its frames run right up to L-1), so only damage in the frames
         * touching the boundary can hide one. Walk down from L-1, skipping the
         * legitimately interleaved MCN/ISRC frames: the first CRC-good position
         * frame means the boundary is clean prev-track body -> gapless; a
         * CRC-bad frame first means a short pregap could hide -> UNKNOWN. */
        m->pregap_state = ACCUDISC_PREGAP_UNKNOWN;
        for (int32_t lba = L - 1; lba >= scan_lo && lba >= base_lba; lba--) {
            const uint8_t *sub = raw + (size_t)(lba - base_lba) * 96;
            uint8_t qb[12];
            accudisc_q q;
            accudisc_sub_extract_q(sub, qb);
            accudisc_q_parse(qb, &q);
            if (!q.crc_ok)
                break; /* damage abuts the boundary */
            if (q.adr != ACCUDISC_Q_POSITION)
                continue; /* MCN/ISRC: expected, look past it */
            m->pregap_state = ACCUDISC_PREGAP_NONE;
            break;
        }
    }

    return written;
}
