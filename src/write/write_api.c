/* Public recording entry point: accudisc_write().
 *
 * Reads the caller's cdrdao .toc (+ optional raw CD-Text blob), opens the raw
 * audio BIN it references, and drives the DAO burn engine (adsc_write_run).
 * File I/O lives here; the engine proper works from a parsed model + an fd.
 */

#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../internal.h"
#include "../meta/cdtext_blob.h"
#include "../mmc/mmc.h"
#include "write.h"
#include "fifo.h"

/* Read an entire file into a fresh malloc'd buffer. If nul, a terminating
 * '\0' is appended and NOT counted in *out_len (for text like the .toc);
 * *out_len (when non-NULL) is the file's byte length. Caller frees *out. */
static int slurp_file(const char *path, int nul, uint8_t **out, uint32_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return ACCUDISC_ERR_OPEN;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ACCUDISC_ERR_IO;
    }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ACCUDISC_ERR_IO;
    }
    uint8_t *buf = malloc((size_t)n + (nul ? 1u : 0u));
    if (!buf) {
        fclose(f);
        return ACCUDISC_ERR_NOMEM;
    }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return ACCUDISC_ERR_IO;
    }
    fclose(f);
    if (nul)
        buf[n] = '\0';
    if (out_len)
        *out_len = (uint32_t)n;
    *out = buf;
    return ACCUDISC_OK;
}

int adsc_write_load_model(const char *toc_path, const char *cdtext_path,
                          struct adsc_write_toc *out, uint8_t **cdtext_buf,
                          struct adsc_cdtext_info *info,
                          char *err, size_t errcap)
{
    if (!toc_path || !out || !cdtext_buf)
        return ACCUDISC_ERR_INVAL;
    *cdtext_buf = NULL;
    if (info)
        memset(info, 0, sizeof(*info));

    uint8_t *txt = NULL;
    int rc = slurp_file(toc_path, 1, &txt, NULL);
    if (rc != ACCUDISC_OK)
        return rc;
    rc = adsc_toc_parse_cue((const char *)txt, out, err, errcap);
    free(txt);
    if (rc != ACCUDISC_OK)
        return rc;

    /* adsc_toc_parse_cue zeroed the model, so cdtext defaults to none. Attach
     * the blob only when the caller asked for it. B2 (blob validation) will
     * gate here, before the model reaches the burn path. */
    if (cdtext_path) {
        uint8_t *blob = NULL;
        uint32_t len = 0;
        rc = slurp_file(cdtext_path, 0, &blob, &len);
        if (rc != ACCUDISC_OK)
            return rc;
        /* Validate (and repair zero-CRC packs) at intake, before the model can
         * reach the burn path. A bad blob costs an error, never a blank. */
        rc = adsc_cdtext_blob_validate(blob, len, info);
        if (rc != ACCUDISC_OK) {
            free(blob);
            return rc;
        }
        out->cdtext = blob;
        out->cdtext_len = len;
        *cdtext_buf = blob;
    }
    return ACCUDISC_OK;
}

int accudisc_write(accudisc_device *dev, const char *toc_path,
                   const char *bin_path, const accudisc_write_opts *opts,
                   void (*progress)(void *user, uint32_t done, uint32_t total),
                   void *user)
{
    if (!opts)
        return ACCUDISC_ERR_INVAL;

    /* Size negotiation first, on the same rule and for the same reasons as
     * accudisc_read_cdda (engine.c): before the device check, so a binding with
     * a stale layout is diagnosed as ERR_ABI ("rebuild") rather than ERR_INVAL
     * ("fix your arguments"), and so the guard is reachable in tests/test_abi.c
     * without a drive. `opts` is only guaranteed to be opts->size bytes long,
     * so everything downstream reads the local copy, never the caller's.
     *
     * It matters more here than there. This is the one entry point in the
     * library that is not idempotent: reading a field past the end of a short
     * caller's struct does not return a wrong answer to be checked later, it
     * burns a disc. */
    accudisc_write_opts local;
    int abi = adsc_abi_import(&local, sizeof local, opts, opts->size);
    if (abi != ACCUDISC_OK)
        return abi;
    opts = &local;

    if (!dev || !toc_path || !bin_path)
        return ACCUDISC_ERR_INVAL;

    /* Live-write budget, checked HERE: after the ABI import (so a stale binding
     * still gets ERR_ABI, which tells it something actionable) but before the
     * .toc is parsed and before a single command reaches the drive. A refusal
     * must leave the disc and the drive untouched, and the only way to promise
     * that is to refuse before doing anything.
     *
     * Simulate is exempt and deliberately so: it runs with the laser off and
     * skips SEND OPC, so it costs the medium nothing. Making it exempt is what
     * lets a caller who hits the budget switch to --simulate and keep working.
     *
     * Default is 0 = unlimited, so this is inert unless a caller opted in. */
    if (!opts->simulate && dev->wr_budget && dev->wr_live >= dev->wr_budget) {
        adsc_dev_log(dev, "write: REFUSED — live-write budget of %u reached on "
                          "this device handle (%u done). Nothing was written "
                          "and the drive was not commanded. Damage to "
                          "rewritable media accumulates per write pass; this "
                          "bound is the guard against a runaway loop. Use "
                          "--simulate (free), or raise the budget deliberately.",
                     dev->wr_budget, dev->wr_live);
        return ACCUDISC_ERR_WRITE_BUDGET;
    }

    struct adsc_write_toc *toc = malloc(sizeof *toc);
    if (!toc)
        return ACCUDISC_ERR_NOMEM;

    uint8_t *cdtext_buf = NULL;
    struct adsc_cdtext_info cti;
    char parse_err[256] = {0};
    int rc = adsc_write_load_model(toc_path, opts->cdtext_path, toc,
                                   &cdtext_buf, &cti, parse_err,
                                   sizeof parse_err);
    if (rc != ACCUDISC_OK) {
        /* The one place the caller can learn WHICH line was wrong: the return
         * code is ERR_INVAL for every malformed .toc. */
        if (parse_err[0])
            adsc_dev_log(dev, "toc: %s", parse_err);
        free(toc);
        return rc;
    }
    /* The one place pass-through is not byte-for-byte: a pack whose CRC field
     * the drive dropped (all-zero) gets its check field regenerated from the
     * untouched payload. Never silent — see RECORDING_PLAN §11.4. */
    if (cti.crc_recomputed)
        adsc_dev_log(dev, "cdtext: regenerated %u zero CRC field(s) of %u pack(s)"
                          " (payload untouched)",
                     cti.crc_recomputed, cti.npacks);

    /* CD-Text SIZE_INFO vs .toc consistency. A mismatch is a CAVEAT, not a
     * refusal: pass-through writes the blob as given, but the CD-Text describes
     * a different track range than the audio, so warn now and report it on the
     * return so the caller can flag it (CLI exit 3). See RECORDING_PLAN §11.4. */
    int caveat = adsc_cdtext_sizeinfo_mismatch(&cti, toc->ntracks);
    if (caveat)
        adsc_dev_log(dev, "cdtext: SIZE_INFO declares tracks %u-%u but the .toc "
                          "has 1-%d; writing the blob as given — the CD-Text may "
                          "not match the audio",
                     cti.si_first_track, cti.si_last_track, toc->ntracks);

    int bin = open(bin_path, O_RDONLY);
    if (bin < 0) {
        free(cdtext_buf);
        free(toc);
        return ACCUDISC_ERR_OPEN;
    }

    /* DESIGNATED, not positional. The positional form silently reassigns every
     * field when one is inserted, and this struct decides whether a laser
     * fires. `burnproof` reaches here as 0 (AUTO) for any caller whose struct
     * predates it — adsc_abi_import above zero-extends — which is the intended
     * default rather than an accident of layout. */
    struct adsc_burn_opts bo = {
        .simulate  = opts->simulate,
        .byteswap  = opts->byteswap,
        .speed     = opts->speed,
        .burnproof = opts->burnproof,
        /* 0 -> the default, ACCUDISC_FIFO_NONE -> off. A caller whose struct
         * predates the field zero-extends to 0 and is therefore PROTECTED,
         * which is the intended direction for a default that exists to stop
         * coasters. */
        /* CLAMPED, and not only for tidiness. fifo_bytes occupies what 0.26.0
         * left as tail padding, so sizeof is 32 in both and the `size` field
         * cannot distinguish them — a 0.26.0 caller's uninitialised padding
         * arrives here as a buffer size. The version macro is the real guard;
         * this is the belt to it, and it turns "allocate 3 GB and lock it"
         * into a sane ring. */
        .fifo_bytes = opts->fifo_bytes == ACCUDISC_FIFO_NONE ? 0
                    : opts->fifo_bytes
                        ? (opts->fifo_bytes > ACCUDISC_FIFO_MAX_BYTES
                               ? ACCUDISC_FIFO_MAX_BYTES : opts->fifo_bytes)
                    : accudisc_fifo_bytes_for(ACCUDISC_FIFO_DEFAULT_SECONDS,
                                              opts->speed > 0
                                                  ? (unsigned)opts->speed : 8u),
    };
    rc = adsc_write_run(dev, toc, bin, &bo, progress, user);

    close(bin);
    free(cdtext_buf);
    free(toc);
    /* Only promote to a caveat if the burn itself completed; a real failure
     * dominates and keeps its own (negative) code. */
    if (rc == ACCUDISC_OK && caveat)
        return ACCUDISC_WROTE_WITH_CAVEATS;
    return rc;
}
