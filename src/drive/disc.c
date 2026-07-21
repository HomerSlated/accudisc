/* Disc-kind guard: which of burn / rip is legal for the loaded disc.
 *
 * Composes GET CONFIGURATION (current profile), READ DISC INFORMATION (status
 * + erasable) and READ TOC (track census). Every command is a read; nothing
 * here changes drive or disc state. */

#include <string.h>

#include "../mmc/mmc.h"
#include "disc.h"

/* MMC profiles that mean "this is a CD". Anything else — DVD, BD, or no
 * current profile — is out of scope by design. */
#define PROFILE_CD_ROM 0x0008
#define PROFILE_CD_R   0x0009
#define PROFILE_CD_RW  0x000A

/* Profile List. Any GET CONFIGURATION returns the current profile in the
 * 8-byte header (bytes 6-7); the profile list is the canonical thing to ask
 * for when that header is all we want. */
#define ADSC_FEATURE_PROFILE_LIST 0x0000

#define SENSE_ASC_MEDIUM_NOT_PRESENT 0x3a

static int profile_is_cd(uint16_t p)
{
    return p == PROFILE_CD_ROM || p == PROFILE_CD_R || p == PROFILE_CD_RW;
}

static int profile_is_recordable(uint16_t p)
{
    return p == PROFILE_CD_R || p == PROFILE_CD_RW;
}

void adsc_disc_classify(accudisc_disc_probe *p)
{
    if (!p)
        return;

    /* 1. No disc at all. Set by the probe from sense; nothing else can be
     *    known, so it short-circuits everything below. */
    if (p->reason == ACCUDISC_DISC_WHY_NO_MEDIUM) {
        p->kind = ACCUDISC_DISC_NEITHER;
        return;
    }

    /* 2. Not a CD. DVD/BD/unrecognised are out of scope: we cannot rip them as
     *    CD-DA and must not burn them as Red Book. Checked before the track
     *    census because a track count from a non-CD profile means nothing. */
    if (!profile_is_cd(p->profile)) {
        p->kind = ACCUDISC_DISC_NEITHER;
        p->reason = ACCUDISC_DISC_WHY_NOT_CD_PROFILE;
        return;
    }

    /* 3. AUDIO first. Any audio track makes the disc rippable within our
     *    scope — pure CD-DA and the audio half of Mixed Mode alike. This
     *    outranks BLANK deliberately: an audio CD-R that has been written and
     *    left appendable is rippable, not blank. */
    if (p->audio_tracks > 0) {
        p->kind = ACCUDISC_DISC_AUDIO;
        p->reason = ACCUDISC_DISC_WHY_AUDIO;
        return;
    }

    /* 4. BLANK. Recordable media with no session started. Status 1 (open) and
     *    2 (closed) both mean something has been written already. */
    if (profile_is_recordable(p->profile) && p->disc_status == 0) {
        p->kind = ACCUDISC_DISC_BLANK;
        p->reason = ACCUDISC_DISC_WHY_BLANK;
        return;
    }

    /* 5. A CD, but neither rippable-as-audio nor writable-as-blank. Name which
     *    so the caller can say something useful rather than just refusing. */
    p->kind = ACCUDISC_DISC_NEITHER;
    if (p->data_tracks > 0)
        p->reason = (p->disc_status == 2) ? ACCUDISC_DISC_WHY_CLOSED_DATA
                                          : ACCUDISC_DISC_WHY_DATA_CD;
    else if (p->disc_status == 1)
        p->reason = ACCUDISC_DISC_WHY_APPENDABLE;
    else
        p->reason = ACCUDISC_DISC_WHY_UNREADABLE;
}

const char *accudisc_disc_kind_str(unsigned kind)
{
    switch (kind) {
    case ACCUDISC_DISC_NEITHER: return "NEITHER";
    case ACCUDISC_DISC_BLANK:   return "BLANK";
    case ACCUDISC_DISC_AUDIO:   return "AUDIO";
    default:                    return "UNKNOWN";
    }
}

const char *accudisc_disc_reason_str(unsigned reason)
{
    switch (reason) {
    case ACCUDISC_DISC_WHY_AUDIO:          return "audio";
    case ACCUDISC_DISC_WHY_BLANK:          return "blank";
    case ACCUDISC_DISC_WHY_DATA_CD:        return "data_cd";
    case ACCUDISC_DISC_WHY_CLOSED_DATA:    return "closed_data";
    case ACCUDISC_DISC_WHY_APPENDABLE:     return "appendable";
    case ACCUDISC_DISC_WHY_NO_MEDIUM:      return "no_medium";
    case ACCUDISC_DISC_WHY_NOT_CD_PROFILE: return "not_cd_profile";
    case ACCUDISC_DISC_WHY_UNREADABLE:     return "unreadable";
    default:                               return "unknown";
    }
}

const char *accudisc_tray_state_str(unsigned tray)
{
    switch (tray) {
    case ACCUDISC_TRAY_CLOSED: return "closed";
    case ACCUDISC_TRAY_OPEN:   return "open";
    default:                   return "unknown";
    }
}

int accudisc_probe_disc(accudisc_device *dev, accudisc_disc_probe *out)
{
    /* Zeroed: GET CONFIGURATION does not report how many bytes came back, so a
     * short response must degrade to profile 0 ("none/unrecognised") rather
     * than reading whatever was on the stack. */
    uint8_t cfg[32] = {0};
    uint8_t di[34];
    uint32_t dlen = 0;
    accudisc_toc toc;
    int rc;

    if (!dev || !out)
        return ACCUDISC_ERR_INVAL;

    memset(out, 0, sizeof(*out));
    out->disc_status = ACCUDISC_DISC_STATUS_UNKNOWN;
    out->erasable = ACCUDISC_DISC_STATUS_UNKNOWN;
    out->tray = ACCUDISC_TRAY_UNKNOWN;

    /* Current profile. This one answers even with an empty tray (profile 0),
     * so its failure is not by itself evidence of anything. */
    if (adsc_mmc_get_configuration(dev, ADSC_FEATURE_PROFILE_LIST, cfg,
                                   sizeof(cfg)) == ACCUDISC_OK)
        out->profile = (uint16_t)(((unsigned)cfg[6] << 8) | cfg[7]);

    /* Disc status and erasable. This is also the command that tells us there
     * is no disc: MEDIUM NOT PRESENT (ASC 0x3A), whose qualifier distinguishes
     * an open tray from a closed empty one. */
    rc = adsc_mmc_read_disc_info(dev, di, sizeof(di), &dlen);
    if (rc == ACCUDISC_OK && dlen >= 3) {
        out->disc_status = di[2] & 0x03;
        out->erasable = (uint8_t)((di[2] >> 4) & 1);
    } else {
        accudisc_sense s;

        accudisc_last_sense(dev, &s);
        if (s.asc == SENSE_ASC_MEDIUM_NOT_PRESENT) {
            out->reason = ACCUDISC_DISC_WHY_NO_MEDIUM;
            /* ASCQ 0x02 = tray open, 0x01 = tray closed; 0x00 unspecified. */
            if (s.ascq == 0x02)
                out->tray = ACCUDISC_TRAY_OPEN;
            else if (s.ascq == 0x01)
                out->tray = ACCUDISC_TRAY_CLOSED;
            adsc_disc_classify(out);
            return ACCUDISC_OK;
        }
        /* Any other failure is survivable: fall through and let the track
         * census and profile decide. */
    }

    /* Track census. A blank disc legitimately has no TOC, so a failure here is
     * not an error — it simply leaves both counts at zero. Format 0 is used
     * deliberately: this guard must stay cheap, and the full TOC would add the
     * lead-in penalty for session data the verdict does not use. */
    if (accudisc_read_toc(dev, &toc) == ACCUDISC_OK) {
        for (uint8_t i = 0; i < toc.track_count; i++) {
            if (ACCUDISC_TRACK_IS_AUDIO(&toc.tracks[i])) {
                if (out->audio_tracks < 0xff)
                    out->audio_tracks++;
            } else if (out->data_tracks < 0xff) {
                out->data_tracks++;
            }
        }
    }

    adsc_disc_classify(out);
    return ACCUDISC_OK;
}
