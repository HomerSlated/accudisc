#include "format.h"

void adsc_cli_fmt_toc(FILE *out, const accudisc_toc *toc,
                      const accudisc_toc_info *info)
{
    for (uint8_t i = 0; i < toc->track_count; i++) {
        const accudisc_track *t = &toc->tracks[i];
        /* session is appended, never inserted: the first five fields are the
         * frozen contract and stay where existing parsers expect them. */
        fprintf(out, "track %u lba %u sectors %u %s", t->number, t->lba,
                t->sectors, ACCUDISC_TRACK_IS_AUDIO(t) ? "audio" : "data");
        if (t->session)
            fprintf(out, " session %u", t->session);
        /* Appended, and only when non-zero: sectors before this track's INDEX
         * 01 that belong to it (ECMA-130 §20). TOC-derivable only for the
         * first track, where the program area's start at LBA 0 supplies the
         * other edge. This is a COUNT OF SECTORS; the `subq_indices=` token
         * below is a statement about ACQUISITION and answers a different
         * question. (Until 2026-07-25 that token was spelled `pregaps=`, which
         * made `pregap 33 ... pregaps=none` read as a self-contradiction.) */
        if (t->pregap)
            fprintf(out, " pregap %u", t->pregap);
        fputc('\n', out);
    }
    for (uint8_t i = 0; i < toc->session_count; i++) {
        const accudisc_session *s = &toc->sessions[i];
        fprintf(out, "session %u tracks %u-%u audio %u data %u leadout %u\n",
                s->number, s->first_track, s->last_track, s->audio_tracks,
                s->data_tracks, s->leadout_lba);
    }
    fprintf(out, "leadout lba %u\n", toc->leadout_lba);

    /* Acquisition path. `subq_indices` says whether THIS command collected
     * INDEX data from the subchannel — never how many pregap sectors exist.
     * It is always `none` here: INDEX 00 lives in the program-area Q
     * subchannel, never in the lead-in, so no READ TOC format can supply it.
     * The `pregaps` SUBCOMMAND is what scans for it. */
    fprintf(out, "source=%s degrade=%s subq_indices=none",
            accudisc_toc_source_str(info->source),
            accudisc_toc_degrade_str(info->degrade));
    if (info->source == ACCUDISC_TOC_SRC_FULLTOC)
        fprintf(out, " sessions=%u..%u disc_type=0x%02x", info->first_session,
                info->last_session, info->disc_type);
    /* A COUNT, not a range — the distinction matters. On a degrade this is the
     * only session structure still reachable, and it comes from a different
     * opcode (READ DISC INFORMATION) than the TOC. 0 = nobody could say. */
    fprintf(out, " session_count=%u", info->session_count);
    /* Structural defects in the lead-in, as a comma-separated slug list.
     * Absent entirely on a well-formed disc, so nothing changes for the
     * overwhelmingly common case; present it means the TOC contradicts itself
     * and is most likely copy-protected. */
    if (toc->anomalies) {
        int first = 1;

        fprintf(out, " anomalies=");
        for (unsigned b = 0; b < 16; b++) {
            if (!(toc->anomalies & (1u << b)))
                continue;
            fprintf(out, "%s%s", first ? "" : ",",
                    accudisc_toc_anomaly_str(1u << b));
            first = 0;
        }
        if (toc->anomalies & ACCUDISC_TOC_ANOM_UNTRUSTED_GEOMETRY)
            fprintf(out, " toc_trusted=0");
    }
    fputc('\n', out);
}
