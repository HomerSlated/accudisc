/* Disc-kind verdict logic over synthetic (profile, status, track census)
 * inputs. Pure: no device, no hardware. */

#include <assert.h>
#include <string.h>

#include "drive/disc.h"

#define CD_ROM 0x0008
#define CD_R   0x0009
#define CD_RW  0x000A
#define DVD_R  0x0011

/* Classify one synthetic disc. status/erasable take
 * ACCUDISC_DISC_STATUS_UNKNOWN when the probe could not obtain them. */
static accudisc_disc_probe verdict(uint16_t profile, uint8_t status,
                                   uint8_t audio, uint8_t data)
{
    accudisc_disc_probe p;

    memset(&p, 0, sizeof(p));
    p.profile = profile;
    p.disc_status = status;
    p.audio_tracks = audio;
    p.data_tracks = data;
    adsc_disc_classify(&p);
    return p;
}

int main(void)
{
    accudisc_disc_probe p;

    /* --- AUDIO: the rip path ---------------------------------------------- */

    /* Pressed CD-DA. */
    p = verdict(CD_ROM, 2, 12, 0);
    assert(p.kind == ACCUDISC_DISC_AUDIO);
    assert(p.reason == ACCUDISC_DISC_WHY_AUDIO);

    /* Burned audio CD-R, closed — the everyday case. */
    p = verdict(CD_R, 2, 10, 0);
    assert(p.kind == ACCUDISC_DISC_AUDIO);

    /* Audio CD-RW. Erasable media carrying audio is still a rip, not a burn. */
    p = verdict(CD_RW, 2, 10, 0);
    assert(p.kind == ACCUDISC_DISC_AUDIO);

    /* Mixed Mode: audio plus a data track. In scope for the audio portion. */
    p = verdict(CD_ROM, 2, 11, 1);
    assert(p.kind == ACCUDISC_DISC_AUDIO);

    /* AUDIO OUTRANKS BLANK. A CD-R written with audio but left appendable
     * (status 1) must classify rippable — the precedence rule exists for
     * exactly this disc, and getting it wrong would offer to burn over it. */
    p = verdict(CD_R, 1, 3, 0);
    assert(p.kind == ACCUDISC_DISC_AUDIO);
    assert(p.reason == ACCUDISC_DISC_WHY_AUDIO);

    /* --- BLANK: the burn path --------------------------------------------- */

    p = verdict(CD_R, 0, 0, 0);
    assert(p.kind == ACCUDISC_DISC_BLANK);
    assert(p.reason == ACCUDISC_DISC_WHY_BLANK);

    p = verdict(CD_RW, 0, 0, 0);
    assert(p.kind == ACCUDISC_DISC_BLANK);

    /* A pressed disc is never blank however empty its TOC looks: CD-ROM is
     * not recordable, so status 0 cannot make it burnable. */
    p = verdict(CD_ROM, 0, 0, 0);
    assert(p.kind == ACCUDISC_DISC_NEITHER);

    /* Status 1 (open session) and 2 (closed) both mean "written already". */
    p = verdict(CD_R, 1, 0, 0);
    assert(p.kind == ACCUDISC_DISC_NEITHER);
    assert(p.reason == ACCUDISC_DISC_WHY_APPENDABLE);
    p = verdict(CD_R, 2, 0, 1);
    assert(p.kind == ACCUDISC_DISC_NEITHER);
    assert(p.reason == ACCUDISC_DISC_WHY_CLOSED_DATA);

    /* --- NEITHER: refuse, and say which kind of refusal ------------------- */

    /* Data-only CD, open session. */
    p = verdict(CD_R, 1, 0, 1);
    assert(p.kind == ACCUDISC_DISC_NEITHER);
    assert(p.reason == ACCUDISC_DISC_WHY_DATA_CD);

    /* Pressed data CD-ROM. */
    p = verdict(CD_ROM, 2, 0, 1);
    assert(p.kind == ACCUDISC_DISC_NEITHER);
    assert(p.reason == ACCUDISC_DISC_WHY_CLOSED_DATA);

    /* Not a CD at all. Checked before the census, so even a nonsense track
     * count cannot promote a DVD to AUDIO. */
    p = verdict(DVD_R, 0, 0, 0);
    assert(p.kind == ACCUDISC_DISC_NEITHER);
    assert(p.reason == ACCUDISC_DISC_WHY_NOT_CD_PROFILE);
    p = verdict(DVD_R, 2, 9, 0);
    assert(p.kind == ACCUDISC_DISC_NEITHER);
    assert(p.reason == ACCUDISC_DISC_WHY_NOT_CD_PROFILE);

    /* Profile 0 = no current profile / unrecognised medium. */
    p = verdict(0x0000, ACCUDISC_DISC_STATUS_UNKNOWN, 0, 0);
    assert(p.kind == ACCUDISC_DISC_NEITHER);
    assert(p.reason == ACCUDISC_DISC_WHY_NOT_CD_PROFILE);

    /* A CD that answered nothing: profile says CD, but no status and no
     * tracks. Distinct from "not a CD" and from "blank". */
    p = verdict(CD_ROM, ACCUDISC_DISC_STATUS_UNKNOWN, 0, 0);
    assert(p.kind == ACCUDISC_DISC_NEITHER);
    assert(p.reason == ACCUDISC_DISC_WHY_UNREADABLE);

    /* --- no medium short-circuits everything ------------------------------ */

    /* The probe sets NO_MEDIUM from sense before classifying; nothing else can
     * be known, so no other input may override it. */
    memset(&p, 0, sizeof(p));
    p.reason = ACCUDISC_DISC_WHY_NO_MEDIUM;
    p.profile = CD_R;
    p.disc_status = 0; /* would otherwise read as BLANK */
    adsc_disc_classify(&p);
    assert(p.kind == ACCUDISC_DISC_NEITHER);
    assert(p.reason == ACCUDISC_DISC_WHY_NO_MEDIUM);

    memset(&p, 0, sizeof(p));
    p.reason = ACCUDISC_DISC_WHY_NO_MEDIUM;
    p.audio_tracks = 10; /* would otherwise read as AUDIO */
    adsc_disc_classify(&p);
    assert(p.kind == ACCUDISC_DISC_NEITHER);
    assert(p.reason == ACCUDISC_DISC_WHY_NO_MEDIUM);

    /* NULL must not crash. */
    adsc_disc_classify(NULL);

    /* --- machine tokens are stable ---------------------------------------- */

    assert(!strcmp(accudisc_disc_kind_str(ACCUDISC_DISC_NEITHER), "NEITHER"));
    assert(!strcmp(accudisc_disc_kind_str(ACCUDISC_DISC_BLANK), "BLANK"));
    assert(!strcmp(accudisc_disc_kind_str(ACCUDISC_DISC_AUDIO), "AUDIO"));
    assert(!strcmp(accudisc_disc_kind_str(99), "UNKNOWN"));

    assert(!strcmp(accudisc_disc_reason_str(ACCUDISC_DISC_WHY_AUDIO), "audio"));
    assert(!strcmp(accudisc_disc_reason_str(ACCUDISC_DISC_WHY_BLANK), "blank"));
    assert(!strcmp(accudisc_disc_reason_str(ACCUDISC_DISC_WHY_DATA_CD),
                   "data_cd"));
    assert(!strcmp(accudisc_disc_reason_str(ACCUDISC_DISC_WHY_CLOSED_DATA),
                   "closed_data"));
    assert(!strcmp(accudisc_disc_reason_str(ACCUDISC_DISC_WHY_APPENDABLE),
                   "appendable"));
    assert(!strcmp(accudisc_disc_reason_str(ACCUDISC_DISC_WHY_NO_MEDIUM),
                   "no_medium"));
    assert(!strcmp(accudisc_disc_reason_str(ACCUDISC_DISC_WHY_NOT_CD_PROFILE),
                   "not_cd_profile"));
    assert(!strcmp(accudisc_disc_reason_str(ACCUDISC_DISC_WHY_UNREADABLE),
                   "unreadable"));
    assert(!strcmp(accudisc_disc_reason_str(99), "unknown"));

    assert(!strcmp(accudisc_tray_state_str(ACCUDISC_TRAY_OPEN), "open"));
    assert(!strcmp(accudisc_tray_state_str(ACCUDISC_TRAY_CLOSED), "closed"));
    assert(!strcmp(accudisc_tray_state_str(ACCUDISC_TRAY_UNKNOWN), "unknown"));

    return 0;
}
