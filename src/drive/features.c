/* Feature probing: the drive's claims (GET CONFIGURATION) cross-checked with
 * functional smoke reads. Port of c2read probe_features(); the claim-vs-
 * function split exists because drives are known to advertise C2 they don't
 * honour, and to honour C2 they don't advertise. */

#include <stdlib.h>
#include <string.h>

#include "../mmc/mmc.h"

#define ADSC_FEATURE_CD_READ 0x001E
/* CD Mastering — Session/Disc-At-Once, which is the write type this library
 * uses. Its BUF bit is the drive's claim to "zero loss linking", i.e.
 * BURN-Proof / Just-Link / whatever the vendor calls it. MMC-5 5.3.24. */
#define ADSC_FEATURE_CD_MASTERING 0x002E

static int cd_read_feature(struct accudisc_device *dev, accudisc_features *f)
{
    uint8_t buf[64] = {0};

    if (adsc_mmc_get_configuration(dev, ADSC_FEATURE_CD_READ, buf,
                                   sizeof(buf)) != ACCUDISC_OK)
        return -1;
    /* 8-byte feature header, then the first feature descriptor. */
    unsigned code = ((unsigned)buf[8] << 8) | buf[9];
    if (code != ADSC_FEATURE_CD_READ)
        return -1;
    f->feature_present = 1;
    f->current = buf[10] & 0x01;
    f->dap = (buf[12] >> 7) & 1;
    f->c2_claimed = (buf[12] >> 1) & 1;
    f->cdtext_claimed = buf[12] & 1;
    return 0;
}

/* CD Mastering (002Eh): what the drive claims about DAO writing.
 *
 * THIS ONE IS A CLAIM WE CANNOT SMOKE-TEST, and that is why it is reported
 * separately from the functional probes below rather than beside them.
 * Everything else in this file cross-checks an advertisement against a real
 * read, because drives are known to advertise C2 they do not honour. There is
 * no equivalent for buffer-underrun-free recording: proving BUF works means
 * deliberately starving a real burn and inspecting the disc afterwards — one
 * blank per drive, destructively. So `buf_claimed` is acted on and NOT
 * verified, and every consumer must say so rather than print it as a fact.
 *
 * `mastering_current` is a SEPARATE question from `buf_claimed`: Current means
 * "active for the loaded medium" (MMC-5 5.2.2.4), so a drive that can do this
 * reports Current=0 with a finished disc in the tray and Current=1 with a
 * blank. Measured on the PX-716A 2026-08-27: byte12=0x7F (BUF=1 SAO=1 RawMS=1
 * Raw=1 TestWrite=1) with Current=0 against a burnt disc. Reading the two as
 * one question would refuse BURN-Proof on every burn. */
int adsc_probe_cd_mastering(struct accudisc_device *dev,
                            accudisc_features *f)
{
    uint8_t buf[64] = {0};

    if (adsc_mmc_get_configuration(dev, ADSC_FEATURE_CD_MASTERING, buf,
                                   sizeof(buf)) != ACCUDISC_OK)
        return -1;
    if ((((unsigned)buf[8] << 8) | buf[9]) != ADSC_FEATURE_CD_MASTERING)
        return -1;   /* absent: CDEmu answers exactly this way */
    f->mastering_present = 1;
    f->mastering_current = buf[10] & 0x01;
    f->buf_claimed  = (buf[12] >> 6) & 1;
    f->sao_claimed  = (buf[12] >> 5) & 1;
    f->test_write_claimed = (buf[12] >> 2) & 1;
    return 0;
}

/* Does READ CD with this C2/sub combination return data (not CHECK
 * CONDITION)? Three CD-DA sectors from `lba`. */
static int combo_smoke(struct accudisc_device *dev, uint32_t lba, unsigned c2,
                       unsigned sub)
{
    uint32_t sector_len = adsc_read_cd_sector_len(c2, sub);
    uint8_t *buf = malloc((size_t)3 * sector_len);
    int rc;

    if (!buf)
        return 0;
    rc = adsc_mmc_read_cd(dev, lba, 3, ADSC_SECTOR_CDDA, c2, sub, buf,
                          sector_len);
    free(buf);
    return rc == ACCUDISC_OK;
}

/* Accurate Stream probe: read [lba, lba+12), then re-read from staggered
 * start points with cache defeat in between; on an Accurate Stream drive
 * the overlapping sectors are byte-identical regardless of where the read
 * began. Any positional mismatch = the drive can slip.
 *
 * SCOPE OF THE VERDICT, and it is narrower than "Accurate Stream" sounds.
 * The MMC capability is a read-only bit in mode page 0x2A (MMC-5 Annex E.11:
 * the page "is read only", legacy, last defined in MMC-3) — so as a *claim* it
 * is immutable and speed-independent. What this function returns is not that
 * claim; it is an observation of behaviour, taken at whatever speed the drive
 * happens to be running, since we pass none. Nothing here establishes that the
 * behaviour is speed-invariant: a drive that holds position at 4x and slips at
 * 40x is one immutable bit with two probe answers, and this probe would report
 * whichever speed it met. Read the result as "did not slip, here, at this
 * rate", not as the drive's page-0x2A capability. (Raised by cdda2img
 * 2026-08-31, off the back of the RECOVERY.md §12.10 governor finding; recorded
 * as a bound on the verdict, not as a call for a speed sweep.)
 *
 * WHAT IT ESTABLISHES THAT READING THE BIT WOULD NOT. MMC-3 §6.3.11 defines
 * the bit as the drive supporting "an audio location without losing place to
 * continue the READ CD command", and the READ CD section gives the reason:
 * CD-DA carries no sector header, so there is a "1-second uncertainty of the
 * address" and "reissuing the command may not return exactly the same data as
 * the previous try". That is exactly the slip the memcmp below detects. But
 * the spec's remedy for a drive that lacks the capability is a HARD error
 * (ABORTED COMMAND / READ ERROR - LOSS OF STREAMING), and that path is scoped
 * to the stream being lost mid-read — "If the Logical Unit stops while
 * streaming". A fresh seek is a different case, and the standard offers only
 * the uncertainty sentence for it. So one bit covers two situations, and a
 * drive can be fully compliant on the mid-read error path while still slipping
 * on a re-seek. The re-seek half is the one ripping depends on and the one
 * nothing forces the drive to report honestly — which is why this probe is
 * worth running even on a drive whose page 0x2A asserts the bit. */
#define AS_SPAN 12

int accudisc_probe_accurate_stream(accudisc_device *dev, uint32_t lba,
                                   uint8_t *accurate)
{
    static const uint32_t starts[] = {1, 5, 9};
    uint32_t sec = ACCUDISC_BYTES_AUDIO;
    uint8_t *base, *shifted;
    int rc = ACCUDISC_OK;

    if (!dev || !accurate)
        return ACCUDISC_ERR_INVAL;
    base = malloc((size_t)AS_SPAN * sec);
    shifted = malloc((size_t)AS_SPAN * sec);
    if (!base || !shifted) {
        rc = ACCUDISC_ERR_NOMEM;
        goto out;
    }

    rc = adsc_mmc_read_cd(dev, lba, AS_SPAN, ADSC_SECTOR_CDDA,
                          ADSC_C2_NONE, ADSC_SUB_NONE, base,
                          sec);
    if (rc != ACCUDISC_OK)
        goto out;

    *accurate = 1;
    for (size_t t = 0; t < sizeof(starts) / sizeof(starts[0]); t++) {
        uint32_t k = starts[t];

        /* Cache defeat: a far throwaway read so the staggered read hits
         * the platter, not the drive's buffer of the base read. */
        adsc_mmc_read_cd(dev, lba + 5000, 1, ADSC_SECTOR_CDDA,
                         ADSC_C2_NONE, ADSC_SUB_NONE, shifted, sec);

        rc = adsc_mmc_read_cd(dev, lba + k, AS_SPAN, ADSC_SECTOR_CDDA,
                              ADSC_C2_NONE, ADSC_SUB_NONE, shifted, sec);
        if (rc != ACCUDISC_OK)
            goto out;
        if (memcmp(base + (size_t)k * sec, shifted,
                   (size_t)(AS_SPAN - k) * sec) != 0) {
            *accurate = 0;
            break;
        }
    }

out:
    free(base);
    free(shifted);
    return rc;
}

/* Where the smoke reads go: the first audio track. LBA 0 on an audio disc, and
 * past the data track on a Mixed Mode one — where LBA 0 would refuse a CD-DA
 * read with 5/64/00 and report a working drive as five failed combos. */
static uint32_t first_audio_lba(accudisc_device *dev)
{
    accudisc_toc toc;

    if (accudisc_read_toc(dev, &toc) != ACCUDISC_OK)
        return 0;
    for (unsigned t = 0; t < toc.track_count; t++)
        if (ACCUDISC_TRACK_IS_AUDIO(&toc.tracks[t]))
            return toc.tracks[t].lba;
    return 0;
}

int accudisc_probe_features(accudisc_device *dev, accudisc_features *out)
{
    if (!dev || !out)
        return ACCUDISC_ERR_INVAL;
    memset(out, 0, sizeof(*out));

    int have_feat = cd_read_feature(dev, out);

    /* Write capability. Its absence is not an error — CDEmu returns no CD
     * Mastering descriptor at all and burns perfectly well through it. */
    (void)adsc_probe_cd_mastering(dev, out);

    /* WHAT IS LOADED DECIDES WHETHER THE SMOKE READS MEAN ANYTHING. They are
     * CD-DA reads; with an empty tray, a blank or a data disc they fail for
     * want of audio, and until 0.47.0 that was reported as the drive failing
     * all five combinations (and, on a blank or data disc, as a confident
     * C2_UNSUPPORTED). So classify first, and do not send reads that can only
     * produce a wrong answer. If the disc cannot be classified at all, fall
     * back to issuing them: an unclassified disc is not evidence of no disc. */
    accudisc_disc_probe dp;
    uint32_t lba = 0;

    if (accudisc_probe_disc(dev, &dp) != ACCUDISC_OK)
        out->medium = ACCUDISC_FEATURES_MEDIUM_UNKNOWN;
    else if (dp.kind == ACCUDISC_DISC_AUDIO)
        out->medium = ACCUDISC_FEATURES_MEDIUM_AUDIO;
    else if (dp.reason == ACCUDISC_DISC_WHY_NO_MEDIUM)
        out->medium = ACCUDISC_FEATURES_MEDIUM_NONE;
    else if (dp.reason == ACCUDISC_DISC_WHY_UNREADABLE ||
             (dp.reason == ACCUDISC_DISC_WHY_NOT_CD_PROFILE && dp.profile == 0))
        /* Not a classification, an absence of one: a drive still evaluating a
         * freshly loaded disc answers profile 0 with a perfectly good audio CD
         * in the tray (measured 2026-09-12). Withholding the reads here would
         * turn "not sure yet" into "no audio". */
        out->medium = ACCUDISC_FEATURES_MEDIUM_UNKNOWN;
    else
        out->medium = ACCUDISC_FEATURES_MEDIUM_NO_AUDIO;

    int smoke = out->medium == ACCUDISC_FEATURES_MEDIUM_AUDIO ||
                out->medium == ACCUDISC_FEATURES_MEDIUM_UNKNOWN;
    int no_medium = 0; /* the reads were refused NOT READY */

    if (out->medium == ACCUDISC_FEATURES_MEDIUM_AUDIO)
        lba = first_audio_lba(dev);

    if (smoke) {
        out->ok_c2 = (uint8_t)combo_smoke(dev, lba, ADSC_C2_294, ADSC_SUB_NONE);

        /* The unclassified path keeps the old guard, widened in 0.47.0 from
         * MEDIUM NOT PRESENT to the whole NOT READY key: a C2 smoke read the
         * drive was not ready to perform — no medium, or still becoming ready
         * — says nothing about C2, so the verdict must be UNVERIFIED rather
         * than a false-negative UNSUPPORTED. */
        if (!out->ok_c2) {
            accudisc_sense s;

            accudisc_last_sense(dev, &s);
            if (s.valid && s.key == 0x02)
                no_medium = 1;
        }

        out->ok_sub_raw = (uint8_t)combo_smoke(dev, lba, ADSC_C2_NONE, ADSC_SUB_RAW);
        out->ok_sub_q = (uint8_t)combo_smoke(dev, lba, ADSC_C2_NONE, ADSC_SUB_Q);
        out->ok_c2_sub_raw = (uint8_t)combo_smoke(dev, lba, ADSC_C2_294, ADSC_SUB_RAW);
        out->ok_c2_sub_q = (uint8_t)combo_smoke(dev, lba, ADSC_C2_294, ADSC_SUB_Q);
    }

    /* Vendor write-speed governor, if a driver is attached to answer. The
     * ERR_UNSUPPORTED case (no driver, or a driver without the slot) leaves
     * governor_known 0 — "we could not ask", which a caller must not read as
     * "the drive has no governor". */
    {
        int gon = 0;
        uint32_t grec = 0;

        if (accudisc_write_governor_get(dev, &gon, &grec) == ACCUDISC_OK) {
            out->governor_known = 1;
            out->governor_on = (uint8_t)(gon ? 1 : 0);
            /* Whether the governor is ON is a drive setting. The rate it
             * recommends is for the loaded medium, and the drive answers with
             * a figure even when the tray is empty — so it is withheld there
             * rather than passed on with no disc behind it. */
            out->governor_recommended_kbps =
                out->medium == ACCUDISC_FEATURES_MEDIUM_NONE ? 0 : grec;
        }
    }

    if (!smoke || no_medium)
        out->c2_verdict = ACCUDISC_C2_UNVERIFIED; /* nothing to smoke-test */
    else if (!out->ok_c2)
        out->c2_verdict = ACCUDISC_C2_UNSUPPORTED;
    else if (have_feat == 0 && out->c2_claimed)
        out->c2_verdict = ACCUDISC_C2_SUPPORTED;
    else
        out->c2_verdict = ACCUDISC_C2_UNVERIFIED;
    return ACCUDISC_OK;
}
