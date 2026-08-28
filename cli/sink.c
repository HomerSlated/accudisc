#include "sink.h"

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static void latch(struct cli_wfail *fail, const char *lane, int e)
{
    if (fail && !fail->err) {
        fail->err = e ? e : EIO;
        fail->lane = lane;
    }
}

int cli_sink_write(FILE *f, const char *lane, const void *p, size_t n,
                   struct cli_wfail *fail)
{
    if (n == 0)
        return 0;
    if (!f) {
        latch(fail, lane, EBADF);
        return -1;
    }
    errno = 0;
    if (fwrite(p, 1, n, f) == n)
        return 0;
    latch(fail, lane, errno);
    return -1;
}

int cli_sink_close(FILE *f, const char *lane, struct cli_wfail *fail)
{
    int rc = 0, e = 0;

    if (!f)
        return 0;

    errno = 0;
    if (fflush(f) != 0) {
        rc = -1;
        e = errno;
    } else {
        struct stat sb;
        int fd = fileno(f);

        if (fd >= 0 && fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode)) {
            errno = 0;
            if (fsync(fd) != 0) {
                rc = -1;
                e = errno;
            }
        }
    }
    /* ALWAYS close, even after a failure above: returning early would leak the
     * descriptor on exactly the path where the tool is about to report trouble
     * and keep running. */
    errno = 0;
    if (fclose(f) != 0 && rc == 0) {
        rc = -1;
        e = errno;
    }
    if (rc) {
        /* errno is set whether or not a latch was supplied. A caller with no
         * `fail` struct (dump_to_file) has strerror(errno) as its only way to
         * say WHY, and stdio leaves errno unspecified after a successful
         * internal step — which printed "FAILED: Success", the exact species
         * of nonsense this whole change exists to remove. */
        errno = e ? e : EIO;
        latch(fail, lane, errno);
    }
    return rc;
}
