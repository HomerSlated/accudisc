#ifndef ADSC_CLI_SINK_H
#define ADSC_CLI_SINK_H

#include <stddef.h>
#include <stdio.h>

/* Checked output writing for the CLI's file lanes, split out of main.c for the
 * same reason format.c was: so it can be exercised without a drive.
 *
 * It exists because the version before it did not check anything. `read_sink`
 * issued four `fwrite` calls and consulted none of them, never looked at
 * `ferror`, and always returned 0; `dump_to_file` wrote and closed blind and
 * then printed a byte count it had not verified. So a full filesystem produced
 * a TRUNCATED output file and exit 0 — the rip "succeeded" over data that is
 * wrong, which is this project's dominant failure shape sitting in the one
 * place that writes the audio.
 *
 * Nothing here touches a device or a global. */

/* The FIRST write failure on any lane, latched. Zero-initialise it. Only the
 * first is kept: once a filesystem is full every subsequent lane fails too, and
 * the last one to fail is not the one worth reporting. */
struct cli_wfail {
    int err;           /* errno, 0 = nothing has failed */
    const char *lane;  /* the option that named the file: "--pcm", "--c2f", … */
};

/* One write. Returns 0, or -1 having latched the failure into *fail.
 * n == 0 is a success and touches nothing. */
int cli_sink_write(FILE *f, const char *lane, const void *p, size_t n,
                   struct cli_wfail *fail);

/* Flush, sync, and close, CHECKING ALL THREE, and latch any failure.
 * Returns 0 or -1. A NULL file is a success (nothing was opened).
 *
 * All three stages matter, for different reasons. `fwrite` can succeed into a
 * userspace buffer that has not reached the kernel; `fflush` pushes it and is
 * where a local ENOSPC usually surfaces; `fclose` is the LAST chance a buffered
 * error has to be reported, and discarding its return is how a short file
 * leaves with exit 0.
 *
 * fsync runs only on REGULAR files, decided by fstat rather than guessed from
 * the path: a pipe or a character device answers EINVAL, and treating that as
 * failure would break every `--pcm /dev/stdout` invocation. */
int cli_sink_close(FILE *f, const char *lane, struct cli_wfail *fail);

#endif /* ADSC_CLI_SINK_H */
