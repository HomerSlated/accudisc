/* Flow control in the DAO burn loop, against a fake drive.
 *
 * WHY THIS FILE EXISTS. Until 0.24.0 write_chunk() was `for (;;)` around
 * WRITE(10), retrying forever while the drive answered "buffer full"
 * (SK 2 / ASC 04 / ASCQ 08) with a 40 ms sleep between tries. A drive that
 * stayed in that state held the burn indefinitely: no error, no log line, no
 * exit, and — since a healthy burn also spends time in exactly that loop —
 * nothing on stdout to tell a wedged drive from a slow one.
 *
 * A cap nobody has watched fire is not a cap, so this compiles the REAL
 * src/write/burn.c against a stub MMC layer and makes it fire. STALL_CAP_MS
 * and STALL_POLL_MS are overridden by the build (tests/CMakeLists.txt) for the
 * same reason ADSC_OFFSETS_DB is: waiting two real minutes to observe the
 * timeout would mean nobody ever runs it.
 *
 * WHAT IS AND IS NOT BEING COUNTED. The stalls here are the drive telling the
 * HOST to wait — the host is ahead, which is the safe direction. An underrun is
 * the opposite and MMC exposes no counter for it, so nothing in this file, and
 * nothing in the engine, claims to detect one. See struct write_flow.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "accudisc/accudisc.h"
#include "internal.h"
#include "write/write.h"

/* ---- the fake drive ------------------------------------------------------ */

static struct {
    unsigned write_calls;      /* WRITE(10) attempts the engine made */
    unsigned busy_left;        /* answer "buffer full" this many more times */
    int      busy_forever;     /* ... or never stop answering it */
    unsigned log_lines;
    char     last_log[512];
    char     gave_up_log[512]; /* the "giving up" line, if it appeared */
    char     flow_log[512];    /* the end-of-burn tally line */
} fake;

static struct accudisc_device *fake_dev(void)
{
    static struct accudisc_device dev;

    memset(&dev, 0, sizeof dev);
    return &dev;
}

/* Set dev->last_sense the way the transport does, then report CHECK CONDITION. */
static int busy(struct accudisc_device *dev)
{
    dev->last_sense.valid = 1;
    dev->last_sense.key = 0x02;
    dev->last_sense.asc = 0x04;
    dev->last_sense.ascq = 0x08;
    return ACCUDISC_ERR_SENSE;
}

int adsc_mmc_write10(struct accudisc_device *dev, int32_t lba, uint32_t nblocks,
                     const uint8_t *buf, uint32_t block_bytes)
{
    (void)lba; (void)nblocks; (void)buf; (void)block_bytes;
    fake.write_calls++;
    if (fake.busy_forever)
        return busy(dev);
    if (fake.busy_left) {
        fake.busy_left--;
        return busy(dev);
    }
    return ACCUDISC_OK;
}

/* Everything else the burn path calls, succeeding quietly. */
int adsc_mmc_send_cue_sheet(struct accudisc_device *d, const uint8_t *c, uint32_t n)
{ (void)d; (void)c; (void)n; return ACCUDISC_OK; }
int adsc_mmc_send_opc(struct accudisc_device *d, int a)
{ (void)d; (void)a; return ACCUDISC_OK; }
int adsc_mmc_sync_cache(struct accudisc_device *d)
{ (void)d; return ACCUDISC_OK; }
int adsc_write_set_params(struct accudisc_device *d, const struct adsc_write_params *w)
{ (void)d; (void)w; return ACCUDISC_OK; }
int adsc_write_read_disc_info(struct accudisc_device *d, struct adsc_disc_info *di)
{ (void)d; memset(di, 0, sizeof *di); di->status = 0; di->leadin_len = 0; return ACCUDISC_OK; }
int adsc_cdtext_blob_validate(const uint8_t *b, uint32_t n, uint32_t *packs)
{ (void)b; (void)n; (void)packs; return ACCUDISC_ERR_INVAL; }
int adsc_cdtext_encode_rw(const uint8_t *p, uint32_t n, uint8_t *out)
{ (void)p; (void)n; (void)out; return ACCUDISC_ERR_INVAL; }
uint32_t adsc_cdtext_rw_block_count(uint32_t packs) { (void)packs; return 0; }

void adsc_dev_log(struct accudisc_device *dev, const char *fmt, ...)
{
    va_list ap;

    (void)dev;
    va_start(ap, fmt);
    vsnprintf(fake.last_log, sizeof fake.last_log, fmt, ap);
    va_end(ap);
    fake.log_lines++;
    if (strstr(fake.last_log, "giving up"))
        snprintf(fake.gave_up_log, sizeof fake.gave_up_log, "%s", fake.last_log);
    if (strstr(fake.last_log, "flow —"))
        snprintf(fake.flow_log, sizeof fake.flow_log, "%s", fake.last_log);
}

/* ---- a minimal one-track burn -------------------------------------------- */

#define TRACK_SECTORS 40u          /* > CHUNK (27), so the loop iterates twice */

static int bin_fd(void)
{
    char path[] = "/var/tmp/accudisc-burnflow-XXXXXX";
    int fd = mkstemp(path);
    static uint8_t z[TRACK_SECTORS * 2352u];

    assert(fd >= 0);
    unlink(path);                  /* it only has to exist as a descriptor */
    assert(write(fd, z, sizeof z) == (ssize_t)sizeof z);
    return fd;
}

static int run(void)
{
    struct adsc_write_toc toc;
    struct adsc_burn_opts opts;
    int fd = bin_fd(), rc;

    memset(&toc, 0, sizeof toc);
    memset(&opts, 0, sizeof opts);
    toc.ntracks = 1;
    toc.leadout_lba = TRACK_SECTORS;
    toc.track[0].audio = 1;
    toc.track[0].sectors = TRACK_SECTORS;
    toc.track[0].file_offset = 0;
    opts.simulate = 1;

    rc = adsc_write_run(fake_dev(), &toc, fd, &opts, NULL, NULL);
    close(fd);
    return rc;
}

static void reset(void) { memset(&fake, 0, sizeof fake); }

/* ---- tests --------------------------------------------------------------- */

static void test_a_clean_burn_never_stalls(void)
{
    reset();
    assert(run() == ACCUDISC_OK);
    assert(fake.write_calls > 0 && "the engine did write something");
    /* The control. Everything below asserts that stalls were COUNTED, which
     * proves nothing unless a burn without them reports none. */
    assert(strstr(fake.flow_log, "0 buffer-full stalls")
           && "a clean burn reports no stalls");
}

static void test_transient_buffer_full_is_survived_and_counted(void)
{
    reset();
    fake.busy_left = 5;            /* five refusals, then it takes the data */
    assert(run() == ACCUDISC_OK && "a busy drive must not fail the burn");
    assert(strstr(fake.flow_log, "5 buffer-full stalls")
           && "every retry was counted");
    /* The retries went to the SAME chunk rather than skipping it: the engine
     * made five extra WRITE(10) calls, not five fewer sectors. */
    assert(fake.write_calls > 5);
}

static void test_a_wedged_drive_is_given_up_on(void)
{
    reset();
    fake.busy_forever = 1;

    /* THE POINT OF THE FILE. Before the cap this call did not return. */
    assert(run() == ACCUDISC_ERR_SENSE
           && "a drive that never accepts data must fail the burn, not hang");
    assert(fake.gave_up_log[0] && "and it must say why in the log");
    assert(strstr(fake.gave_up_log, "buffer full")
           && strstr(fake.gave_up_log, "2/04/08")
           && "the log names the drive's own sense, not a substitute");
}

static void test_the_sense_survives_the_refusal(void)
{
    struct accudisc_device *dev = fake_dev();
    struct adsc_write_toc toc;
    struct adsc_burn_opts opts;
    int fd = bin_fd();

    reset();
    fake.busy_forever = 1;
    memset(&toc, 0, sizeof toc);
    memset(&opts, 0, sizeof opts);
    toc.ntracks = 1;
    toc.leadout_lba = TRACK_SECTORS;
    toc.track[0].audio = 1;
    toc.track[0].sectors = TRACK_SECTORS;
    opts.simulate = 1;

    assert(adsc_write_run(dev, &toc, fd, &opts, NULL, NULL)
           == ACCUDISC_ERR_SENSE);
    close(fd);
    /* No new error code was invented for this: the contract is that the
     * caller reads dev->last_sense and finds the drive's own explanation
     * still there. If a later change clears it, a caller loses the only
     * account of what went wrong. */
    assert(dev->last_sense.valid);
    assert(dev->last_sense.key == 0x02);
    assert(dev->last_sense.asc == 0x04);
    assert(dev->last_sense.ascq == 0x08);
}

int main(void)
{
    test_a_clean_burn_never_stalls();
    test_transient_buffer_full_is_survived_and_counted();
    test_a_wedged_drive_is_given_up_on();
    test_the_sense_survives_the_refusal();
    printf("ok test_burn_flow\n");
    return 0;
}
