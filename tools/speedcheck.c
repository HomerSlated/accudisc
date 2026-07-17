/* End-to-end check of the LIBRARY's SET STREAMING path (adsc_mmc_set_streaming
 * -> adsc_cdb_set_streaming, the builder with the corrected 9-10 length offset).
 * Confirms the drive now honours the library's 0xB6 and the ceiling lands in
 * mode page 2A. SET STREAMING is data-OUT -> needs CAP_SYS_RAWIO. Restores to
 * full speed (explicit descriptor; this drive rejects RDD 0x04). */

#include <stdio.h>

#include <accudisc/accudisc.h>

#include "internal.h"
#include "mmc/mmc.h"

static void show(accudisc_device *dev, const char *tag)
{
    unsigned maxk = 0, curk = 0;
    if (accudisc_get_speed(dev, &maxk, &curk) == ACCUDISC_OK)
        printf("  %-28s page2A cur %.2fx  [max %.2fx]\n", tag,
               curk / 176.4, maxk / 176.4);
    else
        printf("  %-28s (page 2A read failed)\n", tag);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/sr0";
    int err = 0;
    accudisc_device *dev = accudisc_open(path, ACCUDISC_OPEN_RDWR, &err);
    if (!dev) { fprintf(stderr, "open: %s\n", accudisc_strerror(err)); return 1; }

    show(dev, "at open");
    const unsigned rungs[] = { 4, 8, 24, 40 };
    for (unsigned i = 0; i < 4; i++) {
        char tag[40];
        int rc = adsc_mmc_set_streaming(dev, rungs[i], 0, 0xFFFFFFFFu);
        snprintf(tag, sizeof(tag), "mmc_set_streaming %ux rc=%d", rungs[i], rc);
        show(dev, tag);
    }

    accudisc_close(dev);
    return 0;
}
