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
#include "write/fifo.h"
#include "mmc/cdb.h"  /* ADSC_ROTCTL_* — the speed tests assert the CDB's
                     * rotational-control selector, not a copy of it */

/* ---- the fake drive ------------------------------------------------------ */

static struct {
    unsigned write_calls;      /* WRITE(10) attempts the engine made */
    unsigned busy_left;        /* answer "buffer full" this many more times */
    int      busy_forever;     /* ... or never stop answering it */
    int      busy_every;       /* refuse the FIRST attempt of every chunk:
                                * genuine pressure spread across the burn,
                                * as opposed to one long settle at the start */
    int      busy_last_ok;
    unsigned log_lines;
    char     last_log[512];
    char     gave_up_log[512]; /* the "giving up" line, if it appeared */
    char     flow_log[512];    /* the end-of-burn tally line */
    char     buf_log[512];     /* the buffer-fill line, whichever it is */
    char     bp_log[512];      /* the BURN-Proof decision line */
    char     fifo_log[512];    /* the end-of-burn FIFO tally */
    char     prime_log[512];   /* the prefill line */

    /* READ BUFFER CAPACITY behaviour. `cap_refuse` makes the drive reject it
     * outright — the case a real drive without Real-time Streaming presents,
     * and the one no device here can produce (CDEmu answers it and so does the
     * PX-716A), which is exactly why it has to be stubbed. */
    /* CD Mastering (002Eh). `mastering_absent` is CDEmu's real behaviour —
     * measured 2026-08-27, it returns no descriptor at all — and it is the
     * case that decides whether a failover exists behind the host. */
    int      mastering_absent;
    int      buf_claimed;
    int      cue_fail;         /* SEND CUE SHEET refuses: the session never
                                * opens, so there is nothing to abort */
    unsigned sync_calls;       /* FLUSH CACHE (0x35) the engine sent */
    char     abort_log[512];   /* the session-abort line, if it appeared */
    int      cap_refuse;
    uint32_t cap_total;
    uint32_t cap_blank;        /* what the next poll reports as free */
    uint32_t cap_blank_step;   /* added to cap_blank each poll: a draining drive */
    unsigned cap_polls;

    /* WRITE SPEED (0.29.0). `min_write_kbps` is a drive that refuses anything
     * slower than itself with ILLEGAL REQUEST / ASC 0x24 instead of rounding
     * up — the behaviour cdrecord's climb exists for, and one no device here
     * can be made to produce on demand. */
    unsigned set_speed_calls;
    uint16_t last_write_kbps;  /* what the last SET CD SPEED asked for */
    uint16_t last_read_kbps;   /* ... and what it did to the READ speed */
    unsigned last_rotctl;
    unsigned min_write_kbps;   /* 0 = accept anything */
    int      set_speed_hard_fail; /* a non-0x24 failure: must NOT be climbed */
    unsigned report_write_kbps;   /* what page 2A answers; 0 = refuse to say */
    int      report_write_fail;
    unsigned cur_read_kbps;    /* what accudisc_get_speed reports */
    char     speed_log[512];   /* the speed line */
    char     fifo_secs_log[512]; /* the FIFO ride-through line */
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
    if (fake.busy_every) {
        /* Alternate: refuse, then accept. Every chunk waits exactly once. */
        fake.busy_last_ok = !fake.busy_last_ok;
        if (!fake.busy_last_ok)
            return busy(dev);
        return ACCUDISC_OK;
    }
    if (fake.busy_left) {
        fake.busy_left--;
        return busy(dev);
    }
    return ACCUDISC_OK;
}

/* Everything else the burn path calls, succeeding quietly. */
int adsc_mmc_send_cue_sheet(struct accudisc_device *d, const uint8_t *c, uint32_t n)
{ (void)d; (void)c; (void)n; return fake.cue_fail ? ACCUDISC_ERR_IO : ACCUDISC_OK; }
int adsc_mmc_send_opc(struct accudisc_device *d, int a)
{ (void)d; (void)a; return ACCUDISC_OK; }
int adsc_mmc_sync_cache(struct accudisc_device *d)
{ (void)d; fake.sync_calls++; return ACCUDISC_OK; }

int adsc_probe_cd_mastering(struct accudisc_device *d, accudisc_features *f)
{
    (void)d;
    if (fake.mastering_absent)
        return -1;
    f->mastering_present = 1;
    f->mastering_current = 1;
    f->buf_claimed = (uint8_t)fake.buf_claimed;
    f->sao_claimed = 1;
    f->test_write_claimed = 1;
    return 0;
}

int adsc_mmc_read_buffer_capacity(struct accudisc_device *d,
                                  uint32_t *total, uint32_t *blank)
{
    (void)d;
    fake.cap_polls++;
    if (fake.cap_refuse)
        return ACCUDISC_ERR_SENSE;
    *total = fake.cap_total;
    *blank = fake.cap_blank;
    if (fake.cap_blank + fake.cap_blank_step <= fake.cap_total)
        fake.cap_blank += fake.cap_blank_step;
    return ACCUDISC_OK;
}
int adsc_write_set_params(struct accudisc_device *d, const struct adsc_write_params *w)
{ (void)d; (void)w; return ACCUDISC_OK; }
int adsc_write_read_disc_info(struct accudisc_device *d, struct adsc_disc_info *di)
{ (void)d; memset(di, 0, sizeof *di); di->status = 0; di->leadin_len = 0; return ACCUDISC_OK; }
int adsc_mmc_set_cd_speed(struct accudisc_device *d, uint16_t read_kbps,
                          uint16_t write_kbps, unsigned rotctl)
{
    (void)d;
    fake.set_speed_calls++;
    fake.last_read_kbps = read_kbps;
    fake.last_write_kbps = write_kbps;
    fake.last_rotctl = rotctl;
    if (fake.set_speed_hard_fail) {
        d->last_sense.valid = 1;
        d->last_sense.key = 0x03;   /* MEDIUM ERROR: nothing to do with speed */
        d->last_sense.asc = 0x11;
        return ACCUDISC_ERR_SENSE;
    }
    if (fake.min_write_kbps && write_kbps < fake.min_write_kbps) {
        d->last_sense.valid = 1;
        d->last_sense.key = 0x05;   /* ILLEGAL REQUEST */
        d->last_sense.asc = 0x24;   /* INVALID FIELD IN CDB */
        return ACCUDISC_ERR_SENSE;
    }
    return ACCUDISC_OK;
}

int adsc_dev_cur_write_kbps(struct accudisc_device *d, unsigned *kbps)
{
    (void)d;
    if (fake.report_write_fail)
        return ACCUDISC_ERR_SHORT;
    *kbps = fake.report_write_kbps;
    return ACCUDISC_OK;
}

int accudisc_get_speed(accudisc_device *d, unsigned *max_kbps, unsigned *cur_kbps)
{
    (void)d;
    if (max_kbps)
        *max_kbps = 0;
    if (cur_kbps)
        *cur_kbps = fake.cur_read_kbps;
    return fake.cur_read_kbps ? ACCUDISC_OK : ACCUDISC_ERR_SHORT;
}

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
    if (strstr(fake.last_log, "FIFO —"))
        snprintf(fake.fifo_log, sizeof fake.fifo_log, "%s", fake.last_log);
    if (strstr(fake.last_log, "primed"))
        snprintf(fake.prime_log, sizeof fake.prime_log, "%s", fake.last_log);
    if (strstr(fake.last_log, "session aborted"))
        snprintf(fake.abort_log, sizeof fake.abort_log, "%s", fake.last_log);
    if (strstr(fake.last_log, "BURN-Proof"))
        snprintf(fake.bp_log, sizeof fake.bp_log, "%s", fake.last_log);
    if (strstr(fake.last_log, "drive buffer"))
        snprintf(fake.buf_log, sizeof fake.buf_log, "%s", fake.last_log);
    /* ACCUMULATES rather than replaces. The engine emits up to two speed lines
     * (a climb/failure note, then the summary) and both match; replacing meant
     * the second silently ate the first, so a test asserting on the note read
     * as a failure of the note rather than of the capture. */
    if (strstr(fake.last_log, "speed —")) {
        size_t used = strlen(fake.speed_log);
        snprintf(fake.speed_log + used, sizeof fake.speed_log - used, "%s%s",
                 used ? " || " : "", fake.last_log);
    }
    if (strstr(fake.last_log, "FIFO ="))
        snprintf(fake.fifo_secs_log, sizeof fake.fifo_secs_log, "%s",
                 fake.last_log);
    if (strstr(fake.last_log, "FIFO duration UNKNOWN"))
        snprintf(fake.fifo_secs_log, sizeof fake.fifo_secs_log, "%s",
                 fake.last_log);
}

/* ---- a minimal one-track burn -------------------------------------------- */

#define TRACK_SECTORS 40u          /* > CHUNK (27), so the loop iterates twice */

static int bin_fd_n(uint32_t sectors)
{
    char path[] = "/var/tmp/accudisc-burnflow-XXXXXX";
    int fd = mkstemp(path);
    size_t n = (size_t)sectors * 2352u;
    uint8_t *z = calloc(1, n);

    assert(fd >= 0 && z);
    unlink(path);                  /* it only has to exist as a descriptor */
    assert(write(fd, z, n) == (ssize_t)n);
    free(z);
    return fd;
}

static int bin_fd(void) { return bin_fd_n(TRACK_SECTORS); }

/* The speed run_n() asks for. A separate global rather than another run_*
 * variant: every existing test must keep running at 0 (= leave the drive
 * alone), which is also the pre-0.29.0 behaviour. */
static int opt_speed;
/* And the FIFO run_n() asks for. 0 = the engine's default. */
static uint32_t opt_fifo;

static int run_n(uint32_t sectors)
{
    struct adsc_write_toc toc;
    struct adsc_burn_opts opts;
    int fd = bin_fd_n(sectors), rc;

    memset(&toc, 0, sizeof toc);
    memset(&opts, 0, sizeof opts);
    toc.ntracks = 1;
    toc.leadout_lba = sectors;
    toc.track[0].audio = 1;
    toc.track[0].sectors = sectors;
    toc.track[0].file_offset = 0;
    opts.simulate = 1;
    opts.speed = opt_speed;
    opts.fifo_bytes = opt_fifo;

    rc = adsc_write_run(fake_dev(), &toc, fd, &opts, NULL, NULL);
    close(fd);
    return rc;
}

static int run(void) { return run_n(TRACK_SECTORS); }

static void reset(void)
{
    memset(&fake, 0, sizeof fake);
    opt_speed = 0;
    opt_fifo = 0;
    /* A healthy drive by default: 4 MiB buffer, nearly full. Tests that care
     * about the buffer override these. */
    fake.cap_total = 4u * 1024u * 1024u;
    fake.cap_blank = 64u * 1024u;
    /* A drive that claims BURN-Proof, like the PX-716A. Tests that care about
     * the failover override it. */
    fake.buf_claimed = 1;
}

/* ---- tests --------------------------------------------------------------- */

static void test_a_clean_burn_never_stalls(void)
{
    reset();
    assert(run() == ACCUDISC_OK);
    assert(fake.write_calls > 0 && "the engine did write something");
    /* The control. Everything below asserts that stalls were COUNTED, which
     * proves nothing unless a burn without them reports none. */
    assert(strstr(fake.flow_log, "0 chunks held off")
           && "a clean burn reports no hold-offs");
}

static void test_transient_buffer_full_is_survived_and_counted(void)
{
    reset();
    fake.busy_left = 5;            /* five refusals, then it takes the data */
    assert(run() == ACCUDISC_OK && "a busy drive must not fail the burn");
    /* Five retries, ONE chunk — so the report must say one event and name the
     * retry count separately. Conflating them is what made every burn on this
     * drive look like sustained pressure. */
    assert(strstr(fake.flow_log, "ONE hold-off") && "five retries of one write");
    assert(strstr(fake.flow_log, "not 5 events"));
    /* The retries went to the SAME chunk rather than skipping it: the engine
     * made five extra WRITE(10) calls, not five fewer sectors. */
    assert(fake.write_calls > 5);
}

static void test_one_long_holdoff_is_reported_as_ONE_event(void)
{
    reset();
    fake.busy_left = 30;    /* thirty refusals, all of the SAME first write */

    assert(run() == ACCUDISC_OK);
    /* THE DEFECT THIS REPLACES. `stalls` counts 40 ms retries, and as the
     * headline it said "326 buffer-full stalls, worst hold-off 13040 ms" on
     * every burn this drive did — which reads as sustained pressure with one
     * bad moment. Measured on the PX-716A 2026-08-28: ONE moment. 329 retries
     * of a single WRITE(10) at LBA -150, the first write of the burn, which
     * the drive would not accept for 13.2 s while it prepared to record.
     *
     * The harm is not that it was alarming. It is that a burn with 326 GENUINE
     * stalls across the disc printed the SAME headline, so the report could not
     * separate the case worth acting on from the one that happens every time. */
    assert(strstr(fake.flow_log, "ONE hold-off"));
    assert(strstr(fake.flow_log, "not 30 events"));
    assert(strstr(fake.flow_log, "LBA -150") && "and where it happened");
}

static void test_stalls_at_several_places_are_NOT_reported_as_one(void)
{
    reset();
    /* The other side of the same coin: the single-event wording must not
     * swallow genuine, spread-out pressure. Refuse once per chunk, many
     * chunks — the shape that actually warrants attention. */
    fake.busy_every = 1;

    assert(run() == ACCUDISC_OK);
    assert(!strstr(fake.flow_log, "ONE hold-off"));
    assert(strstr(fake.flow_log, "chunks held off"));
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

/* ---- BURN-Proof: does a failover exist behind the host? ------------------- */

static void test_burnproof_follows_the_drive_by_default(void)
{
    reset();
    fake.buf_claimed = 1;
    assert(run() == ACCUDISC_OK);
    assert(strstr(fake.bp_log, "enabled"));
    /* CLAIMED, never "verified". Every other capability in this project is
     * cross-checked against a functional probe; BUF cannot be, because the
     * test destroys a blank. The word in the log is the whole distinction. */
    assert(strstr(fake.bp_log, "CLAIMED"));
    assert(strstr(fake.bp_log, "not verified here"));
}

static void test_a_drive_that_does_not_claim_it_does_not_get_it(void)
{
    reset();
    fake.mastering_absent = 1;      /* CDEmu's real behaviour, measured */

    assert(run() == ACCUDISC_OK && "an absent feature is not an error");
    /* THE DEFECT THIS FIXES. Until 0.26.0 the engine asked EVERY drive for
     * BURN-Proof unconditionally, so it could not tell a drive with a failover
     * from one without — and those want opposite responses to an underrun. */
    assert(strstr(fake.bp_log, "OFF"));
    assert(strstr(fake.bp_log, "not claimed by the drive"));
    assert(strstr(fake.bp_log, "no failover behind the host"));
}

static void test_the_caller_can_refuse_the_failover(void)
{
    struct adsc_write_toc toc;
    struct adsc_burn_opts opts;
    int fd;

    reset();
    fake.buf_claimed = 1;           /* the drive HAS it ... */
    memset(&toc, 0, sizeof toc);
    memset(&opts, 0, sizeof opts);
    toc.ntracks = 1;
    toc.leadout_lba = TRACK_SECTORS;
    toc.track[0].audio = 1;
    toc.track[0].sectors = TRACK_SECTORS;
    opts.simulate = 1;
    opts.burnproof = ACCUDISC_BURNPROOF_OFF;   /* ... and the caller says no */

    fd = bin_fd_n(TRACK_SECTORS);
    assert(adsc_write_run(fake_dev(), &toc, fd, &opts, NULL, NULL) == ACCUDISC_OK);
    close(fd);
    assert(strstr(fake.bp_log, "OFF"));
    assert(strstr(fake.bp_log, "disabled by the caller"));
}

static void test_forcing_it_on_an_unclaiming_drive_says_so(void)
{
    struct adsc_write_toc toc;
    struct adsc_burn_opts opts;
    int fd;

    reset();
    fake.mastering_absent = 1;
    memset(&toc, 0, sizeof toc);
    memset(&opts, 0, sizeof opts);
    toc.ntracks = 1;
    toc.leadout_lba = TRACK_SECTORS;
    toc.track[0].audio = 1;
    toc.track[0].sectors = TRACK_SECTORS;
    opts.simulate = 1;
    opts.burnproof = ACCUDISC_BURNPROOF_ON;

    fd = bin_fd_n(TRACK_SECTORS);
    assert(adsc_write_run(fake_dev(), &toc, fd, &opts, NULL, NULL) == ACCUDISC_OK);
    close(fd);
    /* Allowed — firmware can under-report — but never silently. MODE SELECT
     * may refuse, and that refusal is the drive's answer rather than ours. */
    assert(strstr(fake.bp_log, "FORCED"));
}

/* ---- drive-buffer fill --------------------------------------------------- */

static void test_a_refusing_drive_reports_UNKNOWN_not_zero(void)
{
    reset();
    fake.cap_refuse = 1;

    assert(run() == ACCUDISC_OK && "a drive without 0x5C must still burn");
    /* THE CASE NO DEVICE HERE CAN PRODUCE. CDEmu answers READ BUFFER CAPACITY
     * and so does the PX-716A, so the graceful-degradation path has no hardware
     * to exercise it — which is precisely why it is stubbed rather than left to
     * a live run that would silently never reach it.
     *
     * "Unknown" and "0%" are the same number to anything downstream, and one of
     * them says the drive was starving. The report must not offer the number. */
    assert(strstr(fake.buf_log, "UNKNOWN") && "fill must be reported as unknown");
    assert(strstr(fake.buf_log, "not a reading of zero"));
    assert(!strstr(fake.buf_log, "minimum fill"));
}

static void test_a_healthy_drive_reports_a_high_minimum(void)
{
    reset();
    fake.cap_total = 4u * 1024u * 1024u;
    fake.cap_blank = 0;                 /* buffer full, host well ahead */

    assert(run() == ACCUDISC_OK);
    assert(strstr(fake.buf_log, "minimum fill 100%") && "a full buffer reads 100%");
    assert(strstr(fake.buf_log, "0 below 25%"));
}

#define LONG_SECTORS 1080u   /* 40 chunks -> 5 in-loop polls at one in 8 */

static void test_a_draining_drive_is_seen_going_low(void)
{
    reset();
    /* Free space grows every poll: the host is losing. This is the arm that
     * proves the accounting can MOVE — a minimum that is 100% in every test is
     * indistinguishable from a field nobody writes.
     *
     * A LONGER BURN, not a steeper drain. The default track is 40 sectors = 2
     * chunks, which at one poll in 8 yields a single in-loop sample; a single
     * sample cannot show a trend, and steepening the step to force one number
     * would be tuning the fixture until it agreed. 1080 sectors gives five
     * polls and the fill walks 100 -> 75 -> 50 -> 25 -> 0. */
    fake.cap_total = 1000u * 1000u;
    fake.cap_blank = 0;
    fake.cap_blank_step = 250u * 1000u;

    assert(run_n(LONG_SECTORS) == ACCUDISC_OK);
    assert(fake.cap_polls > 1 && "more than the arming probe");
    assert(!strstr(fake.buf_log, "minimum fill 100%"));
    assert(strstr(fake.buf_log, "(upper bound)")
           && "the report must not present the minimum as a guarantee");
    /* It went below the low-water mark and said so with a count. */
    assert(!strstr(fake.buf_log, "0 below 25%"));
}

static void test_a_constant_answer_is_not_a_measurement(void)
{
    reset();
    /* CDEmu, measured 2026-08-27: total=131584 blank=131584 on every one of 11
     * polls, bit-identical, through a burn that completed cleanly. A virtual
     * device has no spindle and no buffer, so it answers the command with a
     * constant — and the constant it picks means "entirely free".
     *
     * Before the varied check, the report called that "minimum fill 0%, 11
     * below 25%": a confident, well-formed description of a drive on the point
     * of underrunning, about a burn that was perfect. That is the failure this
     * project keeps meeting — the output is the right SHAPE and the wrong
     * REFERENT, and nothing downstream can tell.
     *
     * cap_blank_step 0 reproduces it exactly: same pair every poll. */
    fake.cap_total = 131584u;
    fake.cap_blank = 131584u;
    fake.cap_blank_step = 0;

    assert(run_n(LONG_SECTORS) == ACCUDISC_OK);
    assert(strstr(fake.buf_log, "UNRELIABLE"));
    assert(strstr(fake.buf_log, "131584/131584"));
    /* NO NUMBER. The point is not to report a caveated figure, it is to
     * withhold one — a caveat travels less far than a percentage does. */
    assert(!strstr(fake.buf_log, "minimum fill"));
    assert(!strstr(fake.buf_log, "below 25%"));
}

static void test_a_drive_whose_buffer_MOVES_is_still_reported(void)
{
    reset();
    /* The other side of the same coin: the constant check must not swallow a
     * real measurement. Identical setup to the draining test, and it must
     * produce a figure rather than the refusal above. */
    fake.cap_total = 1000u * 1000u;
    fake.cap_blank = 0;
    fake.cap_blank_step = 250u * 1000u;

    assert(run_n(LONG_SECTORS) == ACCUDISC_OK);
    assert(!strstr(fake.buf_log, "UNRELIABLE"));
    assert(strstr(fake.buf_log, "minimum fill"));
}

static void test_the_poll_is_not_issued_for_every_chunk(void)
{
    reset();
    assert(run() == ACCUDISC_OK);
    /* The poll shares the bus with the data. At ~88 chunks/s, one per chunk
     * roughly doubles the command rate on a USB link — measuring the thing
     * hard enough to cause it. TRACK_SECTORS/CHUNK chunks at one poll in 8
     * must be far fewer polls than writes. */
    assert(fake.cap_polls < fake.write_calls);
}

/* ---- the FIFO ------------------------------------------------------------ */

static int run_fifo(uint32_t sectors, uint32_t fifo_bytes)
{
    struct adsc_write_toc toc;
    struct adsc_burn_opts opts;
    int fd = bin_fd_n(sectors), rc;

    memset(&toc, 0, sizeof toc);
    memset(&opts, 0, sizeof opts);
    toc.ntracks = 1;
    toc.leadout_lba = sectors;
    toc.track[0].audio = 1;
    toc.track[0].sectors = sectors;
    opts.simulate = 1;
    opts.fifo_bytes = fifo_bytes;
    opts.speed = opt_speed;

    rc = adsc_write_run(fake_dev(), &toc, fd, &opts, NULL, NULL);
    close(fd);
    return rc;
}

/* Big enough to hold the ENTIRE test burn. The stub drive accepts a chunk
 * instantly, so it is infinitely faster than any real one — against that, a
 * ring smaller than the burn must starve no matter how quick the producer is,
 * and starvation would be a property of the fixture rather than of the code.
 * 1080 sectors is 40 chunks; 4 MiB is 66 slots. */
#define FIFO_WHOLE_BURN (4u * 1024u * 1024u)

static void test_the_fifo_delivers_every_sector(void)
{
    reset();
    assert(run_fifo(LONG_SECTORS, FIFO_WHOLE_BURN) == ACCUDISC_OK);
    /* Every sector reached WRITE(10) — a ring that dropped or duplicated a
     * chunk would still "succeed", and the disc would be silently wrong. */
    assert(fake.write_calls >= LONG_SECTORS / 27u);
}

static void test_priming_stops_a_false_starvation_at_sector_zero(void)
{
    reset();
    assert(run_fifo(LONG_SECTORS, FIFO_WHOLE_BURN) == ACCUDISC_OK);
    /* THE DEFECT THIS CATCHES, measured on the PX-716A before the prefill
     * existed: the opening pop found an empty ring and scored a starvation.
     * Harmless with BURN-Proof; on a drive WITHOUT it the underrun policy
     * stops the burn — so a buffer added to protect unprotected drives would
     * have made burning impossible on precisely those drives, at sector 0,
     * every time. */
    assert(strstr(fake.fifo_log, "0 starvations"));
    assert(strstr(fake.prime_log, "primed"));
}

static void test_end_of_stream_drain_is_not_a_low_water_mark(void)
{
    reset();
    assert(run_fifo(LONG_SECTORS, FIFO_WHOLE_BURN) == ACCUDISC_OK);
    /* The ring necessarily empties at the end because the FILE ran out.
     * Counting that as the low-water mark reported "low-water 1/111" on a run
     * where the producer spent the entire burn blocked on a FULL ring — a
     * well-formed number about the wrong event. */
    assert(!strstr(fake.fifo_log, "low-water 0/"));
    assert(!strstr(fake.fifo_log, "low-water 1/"));
}

static void test_a_starving_ring_STOPS_a_drive_with_no_failover(void)
{
    struct adsc_write_toc toc;
    struct adsc_burn_opts opts;
    int fd, rc;

    reset();
    fake.mastering_absent = 1;      /* no BURN-Proof behind us */
    memset(&toc, 0, sizeof toc);
    memset(&opts, 0, sizeof opts);
    toc.ntracks = 1;
    toc.leadout_lba = LONG_SECTORS;
    toc.track[0].audio = 1;
    toc.track[0].sectors = LONG_SECTORS;
    opts.simulate = 1;
    opts.fifo_bytes = 1;            /* clamped to the 2-slot floor: it WILL dry */

    fd = bin_fd_n(LONG_SECTORS);
    rc = adsc_write_run(fake_dev(), &toc, fd, &opts, NULL, NULL);
    close(fd);

    /* KEITH'S POLICY, asserted. We own the pipeline. With nothing behind us a
     * dry ring means the drive is about to run its own buffer out and the disc
     * is lost, so the burn STOPS and says why — rather than feeding a drive
     * already known to be unable to recover. */
    assert(rc == ACCUDISC_ERR_IO && "no failover + empty ring must stop the burn");
    assert(strstr(fake.bp_log, "no failover behind the host"));
}

static void test_the_same_starvation_is_SURVIVED_with_a_failover(void)
{
    struct adsc_write_toc toc;
    struct adsc_burn_opts opts;
    int fd, rc;

    reset();
    fake.buf_claimed = 1;           /* ... and now there IS something behind us */
    memset(&toc, 0, sizeof toc);
    memset(&opts, 0, sizeof opts);
    toc.ntracks = 1;
    toc.leadout_lba = LONG_SECTORS;
    toc.track[0].audio = 1;
    toc.track[0].sectors = LONG_SECTORS;
    opts.simulate = 1;
    opts.fifo_bytes = 1;            /* the SAME starvation as above */

    fd = bin_fd_n(LONG_SECTORS);
    rc = adsc_write_run(fake_dev(), &toc, fd, &opts, NULL, NULL);
    close(fd);

    /* Identical input, opposite outcome, and the ONLY difference is whether a
     * failover exists. That is the whole reason 0.26.0 had to learn the
     * difference before this ring could be written. */
    assert(rc == ACCUDISC_OK && "BURN-Proof links and the burn completes");
    assert(strstr(fake.fifo_log, "starvations"));
    assert(!strstr(fake.fifo_log, "0 starvations") && "it really did starve");
}

static void test_no_fifo_still_burns(void)
{
    reset();
    /* --no-fifo must be the OLD synchronous path, not a small ring. */
    assert(run_fifo(LONG_SECTORS, 0) == ACCUDISC_OK);
    assert(!fake.fifo_log[0] && "no FIFO line when there is no FIFO");
}

/* The drive holds an open DAO session from SEND CUE SHEET onward. Returning an
 * error without releasing it leaves the drive live and refusing READ DISC
 * INFORMATION / READ ATIP with 5/2C/00 -- measured on a PX-716A on 2026-08-28
 * after a real starvation, and a tray cycle did not clear it. A single FLUSH
 * CACHE did. */
static void test_a_FAILED_burn_releases_the_session_in_the_drive(void)
{
    struct adsc_write_toc toc;
    struct adsc_burn_opts opts;
    int fd, rc;

    reset();
    fake.mastering_absent = 1;      /* no failover, so starvation stops the burn */
    memset(&toc, 0, sizeof toc);
    memset(&opts, 0, sizeof opts);
    toc.ntracks = 1;
    toc.leadout_lba = LONG_SECTORS;
    toc.track[0].audio = 1;
    toc.track[0].sectors = LONG_SECTORS;
    opts.simulate = 1;
    opts.fifo_bytes = 1;            /* clamped to the 2-slot floor: it WILL dry */

    fd = bin_fd_n(LONG_SECTORS);
    rc = adsc_write_run(fake_dev(), &toc, fd, &opts, NULL, NULL);
    close(fd);

    assert(rc == ACCUDISC_ERR_IO && "the burn must still fail");
    /* The point of the test: it failed AND it let the drive go. */
    assert(fake.sync_calls >= 1 && "a failed burn must abort the session (0x35)");
    assert(strstr(fake.abort_log, "FLUSH CACHE"));
}

/* The other half, and the reason the flag is a flag rather than an
 * unconditional flush: if SEND CUE SHEET is refused there is no session, and
 * firing an abort at a drive that never opened one is a command we cannot
 * justify. Without this the test above passes on `always flush`, which is a
 * different behaviour that happens to satisfy it. */
static void test_a_failure_BEFORE_the_cue_sheet_aborts_nothing(void)
{
    struct adsc_write_toc toc;
    struct adsc_burn_opts opts;
    int fd, rc;

    reset();
    fake.cue_fail = 1;
    memset(&toc, 0, sizeof toc);
    memset(&opts, 0, sizeof opts);
    toc.ntracks = 1;
    toc.leadout_lba = TRACK_SECTORS;
    toc.track[0].audio = 1;
    toc.track[0].sectors = TRACK_SECTORS;
    opts.simulate = 1;

    fd = bin_fd_n(TRACK_SECTORS);
    rc = adsc_write_run(fake_dev(), &toc, fd, &opts, NULL, NULL);
    close(fd);

    assert(rc != ACCUDISC_OK && "a refused cue sheet must fail the burn");
    assert(fake.sync_calls == 0 && "no session was opened, so nothing to abort");
    assert(fake.abort_log[0] == 0);
}

static void test_sizing_converts_duration_and_clamps(void)
{
    /* One rule, used by both the flag parser and the engine. 1 s of CD audio
     * is 176400 bytes; at 8x that is 1411200. */
    assert(accudisc_fifo_bytes_for(1.0, 1) == 176400u);
    assert(accudisc_fifo_bytes_for(1.0, 8) == 1411200u);
    /* The cap is not cosmetic: 5 s at 48x is ~42 MB of LOCKED memory, which on
     * a small board is a refusal to start rather than a buffer. */
    assert(accudisc_fifo_bytes_for(5.0, 48) == ACCUDISC_FIFO_MAX_BYTES);
    assert(accudisc_fifo_bytes_for(0.0, 8) == 0);
}


/* ---- write speed (0.29.0) ------------------------------------------------ */

/* THE DEFECT THIS SUITE MISSED FOR THREE VERSIONS. adsc_burn_opts.speed was
 * carried from the CLI to the engine and read by nobody: `--speed 4` sent no
 * command at all, and the drive wrote at whatever it liked (19.4x measured on
 * a PX-716A). Nothing here could see it, because nothing here asserted that a
 * command was SENT — the burn succeeded either way, which is precisely the
 * shape of defect that survives a green suite. */
static void test_the_requested_speed_reaches_the_drive_at_all(void)
{
    reset();
    opt_speed = 4;
    fake.report_write_kbps = 708;
    assert(run() == ACCUDISC_OK);
    assert(fake.set_speed_calls == 1 && "SET CD SPEED was actually issued");
    /* 4 * 177, not 4 * 176.4: the reference rounds up per rung deliberately
     * because drives round down. */
    assert(fake.last_write_kbps == 708);
    assert(fake.last_rotctl == ADSC_ROTCTL_CLV && "a CD write is CLV, not a "
                                                  "CAV ceiling");
}

static void test_asking_for_no_speed_sends_no_command(void)
{
    reset();
    opt_speed = 0;
    assert(run() == ACCUDISC_OK);
    assert(fake.set_speed_calls == 0 && "0 means leave the drive alone");
}

/* The READ speed is a bystander and must stay one. 0xFFFF in that CDB field is
 * not "unchanged", it is "use your maximum" — so a burn would silently retune
 * the drive's read speed, and the setting outlives us. */
static void test_the_read_speed_is_preserved_not_maximised(void)
{
    reset();
    opt_speed = 4;
    fake.cur_read_kbps = 1764;          /* the drive is reading at 10x */
    assert(run() == ACCUDISC_OK);
    assert(fake.last_read_kbps == 1764 && "the read speed was handed back "
                                          "unchanged");

    /* ...and when the drive will not say, 0xFFFF is the only honest answer. */
    reset();
    opt_speed = 4;
    fake.cur_read_kbps = 0;             /* accudisc_get_speed fails */
    assert(run() == ACCUDISC_OK);
    assert(fake.last_read_kbps == 0xFFFF);
}

/* cdrecord's climb: a drive whose minimum write speed is above the request
 * answers ILLEGAL REQUEST / INVALID FIELD IN CDB instead of rounding up. */
static void test_a_drive_that_cannot_write_that_slowly_is_climbed(void)
{
    reset();
    opt_speed = 1;                      /* 177 kB/s */
    fake.min_write_kbps = 1000;         /* ...but it will not go below 1000 */
    assert(run() == ACCUDISC_OK);
    /* 177, 354, 531, 708, 885, 1062 */
    assert(fake.set_speed_calls == 6);
    assert(fake.last_write_kbps == 1062);
    assert(strstr(fake.speed_log, "cannot write that slowly"));
}

/* ...and the climb must be for THAT sense only. Climbing on any failure would
 * turn one real error into nine and then report success. */
static void test_a_failure_that_is_not_about_speed_is_not_climbed(void)
{
    reset();
    opt_speed = 4;
    fake.set_speed_hard_fail = 1;       /* MEDIUM ERROR, not 5/24 */
    assert(run() == ACCUDISC_OK && "a speed failure must not cost the burn");
    assert(fake.set_speed_calls == 1 && "tried once, did not climb");
    assert(strstr(fake.speed_log, "FAILED"));
}

/* THE HEADLINE. The FIFO is sized in SECONDS against the speed the caller
 * REQUESTED. If the drive is at another rate the byte count is unchanged and
 * the protection is not — 3492720 bytes reported as "5.0 s at 4x" was 1.02 s
 * of real ride-through, and nothing said so. */
static void test_the_ride_through_is_reported_against_the_DRIVES_rate(void)
{
    reset();
    opt_speed = 4;
    fake.report_write_kbps = 3422;      /* the drive is really at 19.4x */
    assert(run_fifo(LONG_SECTORS, 3492720u) == ACCUDISC_OK);
    assert(strstr(fake.fifo_secs_log, "NOT the duration the requested speed "
                                      "implies"));

    /* and it must NOT cry wolf when the drive is where it was asked to be */
    reset();
    opt_speed = 4;
    fake.report_write_kbps = 708;
    assert(run_fifo(LONG_SECTORS, 3492720u) == ACCUDISC_OK);
    assert(fake.fifo_secs_log[0] && "the duration is still reported");
    assert(!strstr(fake.fifo_secs_log, "NOT the duration"));
}

/* A drive that will not state a speed leaves the duration UNCOMPUTABLE, and
 * saying so is the whole point: the previous code printed a confident "5.0 s"
 * derived from an assumption it never checked. */
static void test_an_unstated_speed_is_UNKNOWN_rather_than_assumed(void)
{
    reset();
    opt_speed = 0;
    fake.report_write_fail = 1;
    assert(run_fifo(LONG_SECTORS, 3492720u) == ACCUDISC_OK);
    assert(strstr(fake.fifo_secs_log, "FIFO duration UNKNOWN"));
    assert(strstr(fake.speed_log, "would not report a write speed"));
}

int main(void)
{
    test_a_clean_burn_never_stalls();
    test_transient_buffer_full_is_survived_and_counted();
    test_one_long_holdoff_is_reported_as_ONE_event();
    test_stalls_at_several_places_are_NOT_reported_as_one();
    test_a_wedged_drive_is_given_up_on();
    test_the_sense_survives_the_refusal();
    test_the_fifo_delivers_every_sector();
    test_priming_stops_a_false_starvation_at_sector_zero();
    test_end_of_stream_drain_is_not_a_low_water_mark();
    test_a_starving_ring_STOPS_a_drive_with_no_failover();
    test_the_same_starvation_is_SURVIVED_with_a_failover();
    test_no_fifo_still_burns();
    test_a_FAILED_burn_releases_the_session_in_the_drive();
    test_a_failure_BEFORE_the_cue_sheet_aborts_nothing();
    test_sizing_converts_duration_and_clamps();
    test_burnproof_follows_the_drive_by_default();
    test_a_drive_that_does_not_claim_it_does_not_get_it();
    test_the_caller_can_refuse_the_failover();
    test_forcing_it_on_an_unclaiming_drive_says_so();
    test_a_refusing_drive_reports_UNKNOWN_not_zero();
    test_a_healthy_drive_reports_a_high_minimum();
    test_a_draining_drive_is_seen_going_low();
    test_a_constant_answer_is_not_a_measurement();
    test_a_drive_whose_buffer_MOVES_is_still_reported();
    test_the_poll_is_not_issued_for_every_chunk();
    test_the_requested_speed_reaches_the_drive_at_all();
    test_asking_for_no_speed_sends_no_command();
    test_the_read_speed_is_preserved_not_maximised();
    test_a_drive_that_cannot_write_that_slowly_is_climbed();
    test_a_failure_that_is_not_about_speed_is_not_climbed();
    test_the_ride_through_is_reported_against_the_DRIVES_rate();
    test_an_unstated_speed_is_UNKNOWN_rather_than_assumed();
    printf("ok test_burn_flow\n");
    return 0;
}
