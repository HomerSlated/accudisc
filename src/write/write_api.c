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
#include <unistd.h>

#include "../mmc/mmc.h"
#include "write.h"

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
                          struct adsc_write_toc *out, uint8_t **cdtext_buf)
{
    if (!toc_path || !out || !cdtext_buf)
        return ACCUDISC_ERR_INVAL;
    *cdtext_buf = NULL;

    uint8_t *txt = NULL;
    int rc = slurp_file(toc_path, 1, &txt, NULL);
    if (rc != ACCUDISC_OK)
        return rc;
    rc = adsc_toc_parse_cue((const char *)txt, out);
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
    if (!dev || !toc_path || !bin_path || !opts)
        return ACCUDISC_ERR_INVAL;

    struct adsc_write_toc *toc = malloc(sizeof *toc);
    if (!toc)
        return ACCUDISC_ERR_NOMEM;

    uint8_t *cdtext_buf = NULL;
    int rc = adsc_write_load_model(toc_path, opts->cdtext_path, toc, &cdtext_buf);
    if (rc != ACCUDISC_OK) {
        free(toc);
        return rc;
    }

    int bin = open(bin_path, O_RDONLY);
    if (bin < 0) {
        free(cdtext_buf);
        free(toc);
        return ACCUDISC_ERR_OPEN;
    }

    struct adsc_burn_opts bo = { opts->simulate, opts->byteswap, opts->speed };
    rc = adsc_write_run(dev, toc, bin, &bo, progress, user);

    close(bin);
    free(cdtext_buf);
    free(toc);
    return rc;
}
