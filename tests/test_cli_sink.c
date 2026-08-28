/* The CLI's checked output writing.
 *
 * The defect this exists for: read_sink issued four fwrite calls and checked
 * none of them, dump_to_file wrote and closed blind and then printed a byte
 * count it had not verified, and no fclose return was ever consulted. A full
 * filesystem produced a TRUNCATED file and exit 0 — "the rip succeeded" over
 * data that is wrong.
 *
 * /dev/full is the fixture: it accepts open(), fails every write with ENOSPC,
 * and needs no drive, no disc and no root. Buffered AND unbuffered are both
 * exercised deliberately — with buffering on, a small fwrite SUCCEEDS and the
 * failure does not appear until flush/close, which is exactly why checking
 * fwrite alone would not have been enough.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sink.h"

static FILE *full_unbuffered(void)
{
    FILE *f = fopen("/dev/full", "wb");

    assert(f && "/dev/full opens");
    setvbuf(f, NULL, _IONBF, 0);
    return f;
}

/* ---- writes -------------------------------------------------------------- */

static void test_a_failed_write_is_seen_and_named(void)
{
    struct cli_wfail wf = {0};
    FILE *f = full_unbuffered();

    assert(cli_sink_write(f, "--pcm", "xxxx", 4, &wf) == -1);
    assert(wf.err == ENOSPC && "reported as ENOSPC");
    assert(wf.lane && strcmp(wf.lane, "--pcm") == 0 && "names its lane");
    fclose(f);
}

static void test_a_good_write_reports_nothing(void)
{
    char path[] = "/tmp/adsc_sink_XXXXXX";
    struct cli_wfail wf = {0};
    int fd = mkstemp(path);
    FILE *f;

    assert(fd >= 0);
    f = fdopen(fd, "wb");
    assert(f);
    assert(cli_sink_write(f, "--pcm", "xxxx", 4, &wf) == 0);
    assert(wf.err == 0 && "a good write latches nothing");
    assert(cli_sink_close(f, "--pcm", &wf) == 0);
    assert(wf.err == 0 && "and a good close latches nothing");
    unlink(path);
}

/* The FIRST failure is the one worth reporting: once the filesystem is full
 * every later lane fails too, and the last to fail is not the cause. */
static void test_the_first_failure_is_the_one_kept(void)
{
    struct cli_wfail wf = {0};
    FILE *f = full_unbuffered();

    assert(cli_sink_write(f, "--pcm", "x", 1, &wf) == -1);
    assert(cli_sink_write(f, "--c2f", "x", 1, &wf) == -1);
    assert(strcmp(wf.lane, "--pcm") == 0 && "the FIRST lane, not the last");
    fclose(f);
}

/* c2_len and sub_len are 0 on a plain audio read. A zero-length write must not
 * manufacture an error out of a lane that was never asked for. */
static void test_a_zero_length_write_is_not_a_failure(void)
{
    struct cli_wfail wf = {0};
    FILE *f = full_unbuffered();

    assert(cli_sink_write(f, "--c2f", "", 0, &wf) == 0);
    assert(wf.err == 0);
    fclose(f);
}

/* ---- closes -------------------------------------------------------------- */

/* THE CASE THAT MOTIVATES CHECKING CLOSE AT ALL. With normal buffering a small
 * fwrite to /dev/full SUCCEEDS — the bytes only reach the device at flush. A
 * tool checking fwrite alone sees nothing wrong here. */
static void test_a_buffered_failure_surfaces_only_at_close(void)
{
    struct cli_wfail wf = {0};
    FILE *f = fopen("/dev/full", "wb");

    assert(f);
    assert(cli_sink_write(f, "--pcm", "xxxx", 4, &wf) == 0 &&
           "the BUFFERED write succeeds; nothing reached the device");
    assert(wf.err == 0 && "so nothing is latched yet");
    assert(cli_sink_close(f, "--pcm", &wf) == -1 &&
           "the close is where it finally fails");
    assert(wf.err == ENOSPC);
    assert(strcmp(wf.lane, "--pcm") == 0);
}

/* WHAT THIS FILE CANNOT DISTINGUISH, recorded so nobody re-derives it.
 *
 * Mutation-tested 2026-08-28. Dropping the fflush check alone PASSES, and so
 * does dropping the fclose check alone — on /dev/full the two are redundant,
 * because fflush pushes everything and fclose's internal flush is then a no-op.
 * Only dropping BOTH fails the suite.
 *
 * That is a property of the fixture, not evidence that one of the checks is
 * dead. The case that separates them is a filesystem where the error surfaces
 * only at close — NFS being the standard example — and /dev/full cannot
 * produce it. Both checks stay, and this comment is the reason: a later
 * mutation run will find the same two survivors and should not "clean up" a
 * check on that evidence.
 *
 * (A check is only worth what its inputs can distinguish. Here the inputs
 * cannot, and saying so beats a test that implies otherwise.) */

/* fsync answers EINVAL on a pipe. Treating that as failure would break every
 * `--pcm /dev/stdout` invocation, so the sync is gated on S_ISREG. */
static void test_closing_a_pipe_is_not_a_failure(void)
{
    struct cli_wfail wf = {0};
    int fd[2];
    FILE *w;

    assert(pipe(fd) == 0);
    w = fdopen(fd[1], "wb");
    assert(w);
    assert(cli_sink_write(w, "--pcm", "xxxx", 4, &wf) == 0);
    assert(cli_sink_close(w, "--pcm", &wf) == 0 &&
           "closing a pipe is NOT an error despite fsync refusing it");
    assert(wf.err == 0);
    close(fd[0]);
}

/* Every exit path closes all four lanes unconditionally; three are NULL on an
 * ordinary rip. */
static void test_closing_nothing_is_not_a_failure(void)
{
    struct cli_wfail wf = {0};

    assert(cli_sink_close(NULL, "--c2f", &wf) == 0);
    assert(wf.err == 0);
}

/* A caller may pass no latch at all (dump_to_file does). errno must still say
 * why, or the message becomes "FAILED: Success". */
static void test_a_close_with_no_latch_still_sets_errno(void)
{
    FILE *f = fopen("/dev/full", "wb");

    assert(f);
    assert(cli_sink_write(f, "--x", "xxxx", 4, NULL) == 0);
    errno = 0;
    assert(cli_sink_close(f, "--x", NULL) == -1);
    assert(errno == ENOSPC && "errno survives for a caller with no wfail");
}

int main(void)
{
    test_a_failed_write_is_seen_and_named();
    test_a_good_write_reports_nothing();
    test_the_first_failure_is_the_one_kept();
    test_a_zero_length_write_is_not_a_failure();
    test_a_buffered_failure_surfaces_only_at_close();
    test_closing_a_pipe_is_not_a_failure();
    test_closing_nothing_is_not_a_failure();
    test_a_close_with_no_latch_still_sets_errno();
    printf("ok test_cli_sink\n");
    return 0;
}
