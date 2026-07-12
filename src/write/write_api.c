/* Public recording entry point: accudisc_write().
 *
 * Reads the caller's cdrdao .toc, opens the raw audio BIN it references, and
 * drives the DAO burn engine (adsc_write_run). File I/O lives here; the engine
 * proper works from a parsed model + an fd.
 */

#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../mmc/mmc.h"
#include "write.h"

int accudisc_write(accudisc_device *dev, const char *toc_path,
                   const char *bin_path, const accudisc_write_opts *opts,
                   void (*progress)(void *user, uint32_t done, uint32_t total),
                   void *user)
{
    if (!dev || !toc_path || !bin_path || !opts)
        return ACCUDISC_ERR_INVAL;

    /* Slurp the .toc (small text file). */
    FILE *f = fopen(toc_path, "rb");
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
    char *txt = malloc((size_t)n + 1);
    if (!txt) {
        fclose(f);
        return ACCUDISC_ERR_NOMEM;
    }
    if (fread(txt, 1, (size_t)n, f) != (size_t)n) {
        free(txt);
        fclose(f);
        return ACCUDISC_ERR_IO;
    }
    txt[n] = '\0';
    fclose(f);

    struct adsc_write_toc *toc = malloc(sizeof *toc);
    if (!toc) {
        free(txt);
        return ACCUDISC_ERR_NOMEM;
    }
    int rc = adsc_toc_parse_cue(txt, toc);
    free(txt);
    if (rc != ACCUDISC_OK) {
        free(toc);
        return rc;
    }

    int bin = open(bin_path, O_RDONLY);
    if (bin < 0) {
        free(toc);
        return ACCUDISC_ERR_OPEN;
    }

    struct adsc_burn_opts bo = { opts->simulate, opts->byteswap, opts->speed };
    rc = adsc_write_run(dev, toc, bin, &bo, progress, user);

    close(bin);
    free(toc);
    return rc;
}
