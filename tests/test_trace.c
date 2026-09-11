/* The per-command trace (0.36.0).
 *
 * Two halves. The formatter is pinned on synthetic commands, because the case
 * that matters most — a CHECK CONDITION — cannot be produced without a drive,
 * and it is the one a careless formatter gets wrong: until 0.36.0 the
 * transport left scsi_status at 0 on that path, so a trace would have printed
 * "status=0x00" beside a refusal. Then the real path end to end, against
 * /dev/null: a descriptor the library opens happily and on which every SG_IO
 * fails, which is enough to see what the log sink receives, in what order, and
 * that an untraced handle receives nothing.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "trace.h"
#include "transport/transport.h"

static void test_the_opcode_table_and_the_bulk_rule(void)
{
    assert(!strcmp(adsc_op_name(0x54), "SEND OPC INFORMATION"));
    assert(!strcmp(adsc_op_name(0x5D), "SEND CUE SHEET"));
    assert(!strcmp(adsc_op_name(0xE9), "vendor-specific"));
    assert(!strcmp(adsc_op_name(0x7F), "?"));

    /* The six bulk transfers, and nothing that merely moves a lot of data
     * once (a cue sheet, a GET CONFIGURATION) or polls often. */
    assert(adsc_op_is_bulk(0x2A) && adsc_op_is_bulk(0xBE) &&
           adsc_op_is_bulk(0x28) && adsc_op_is_bulk(0xA8) &&
           adsc_op_is_bulk(0xAA) && adsc_op_is_bulk(0xB9));
    assert(!adsc_op_is_bulk(0x5D) && !adsc_op_is_bulk(0x46) &&
           !adsc_op_is_bulk(0x5C) && !adsc_op_is_bulk(0x35));

    assert(!adsc_trace_shows(ADSC_TRACE_OFF, 0x12));
    assert(adsc_trace_shows(ADSC_TRACE_CTRL, 0x12));
    assert(!adsc_trace_shows(ADSC_TRACE_CTRL, 0x2A) &&
           "control level: a successful WRITE(10) is not shown");
    assert(adsc_trace_shows(ADSC_TRACE_DATA, 0x2A));
}

static void test_the_issue_line_names_the_command(void)
{
    adsc_cmd c;
    char line[256];

    memset(&c, 0, sizeof c);
    c.cdb[0] = 0x54; c.cdb[1] = 0x01;
    c.cdb_len = 10;
    c.dir = ADSC_XFER_NONE;
    adsc_trace_fmt_issue(line, sizeof line, 7, 1.5, &c);
    assert(strstr(line, "trace #000007 +1.500000 > "));
    assert(strstr(line, "54 01 00 00 00 00 00 00 00 00  SEND OPC INFORMATION"));
    assert(strstr(line, "no-data timeout=30s") && "0 = the control default");

    c.cdb[0] = 0x5D;
    c.dir = ADSC_XFER_OUT;
    c.buf_len = 192;
    c.timeout_ms = 120000;
    adsc_trace_fmt_issue(line, sizeof line, 8, 2.0, &c);
    assert(strstr(line, "out=192 timeout=120s"));
}

/* THE ONE THAT MATTERS. A refusal must print as a refusal, with the drive's
 * key/asc/ascq AND a SCSI status that is not zero. */
static void test_a_CHECK_CONDITION_prints_its_sense_and_status(void)
{
    adsc_cmd c;
    char line[256];

    memset(&c, 0, sizeof c);
    c.cdb[0] = 0x54;
    c.cdb_len = 10;
    /* fixed-format sense: MEDIUM ERROR / NO SEEK COMPLETE, disc 4a's refusal */
    c.sense[0] = 0x70; c.sense[2] = 0x03; c.sense[7] = 10;
    c.sense[12] = 0x02; c.sense[13] = 0x00;
    c.sense_len = 18;
    c.scsi_status = 0x02;
    c.driver_status = 0x08;
    adsc_trace_fmt_result(line, sizeof line, 9, &c, ACCUDISC_ERR_SENSE, 12.25);
    assert(strstr(line, "trace #000009 < CHECK CONDITION 3/02/00"));
    assert(strstr(line, "status=0x02 host=0x00 driver=0x08"));
    assert(strstr(line, "12.250 ms"));
}

static void test_GOOD_TRANSPORT_and_resid(void)
{
    adsc_cmd c;
    char line[256];

    memset(&c, 0, sizeof c);
    c.cdb[0] = 0x51;
    c.resid = 6;
    adsc_trace_fmt_result(line, sizeof line, 1, &c, ACCUDISC_OK, 0.5);
    assert(strstr(line, "< GOOD  0.500 ms resid=6"));

    /* the bridge wedging: DID_ERROR, no sense */
    memset(&c, 0, sizeof c);
    c.host_status = 0x07;
    adsc_trace_fmt_result(line, sizeof line, 2, &c, ACCUDISC_ERR_IO, 20000.0);
    assert(strstr(line, "TRANSPORT FAILURE, no sense: host=0x07"));

    /* an outright ioctl failure has no residual to report, whatever the
     * field holds */
    memset(&c, 0, sizeof c);
    c.io_errno = 25;            /* ENOTTY */
    c.resid = 99;
    adsc_trace_fmt_result(line, sizeof line, 3, &c, ACCUDISC_ERR_IO, 0.1);
    assert(strstr(line, "TRANSPORT FAILURE, no sense: ioctl:"));
    assert(!strstr(line, "resid"));
}

/* ---- end to end, through the log sink ------------------------------------ */

static char got[8192];
static unsigned lines;

static void sink(void *user, const char *msg)
{
    size_t used = strlen(got);

    (void)user;
    lines++;
    snprintf(got + used, sizeof got - used, "%s\n", msg);
}

static void test_a_traced_handle_names_every_command_it_sends(void)
{
    accudisc_drive_id id;
    int err = 0;
    accudisc_device *dev = accudisc_open("/dev/null", ACCUDISC_OPEN_TRACE, &err);

    assert(dev && "/dev/null opens; it is the commands that fail");
    got[0] = 0;
    lines = 0;
    accudisc_set_log(dev, sink, NULL);
    assert(accudisc_drive_identify(dev, &id) != ACCUDISC_OK);
    /* INQUIRY, issued: numbered, named, CDB in hex */
    assert(strstr(got, "trace #000001 +"));
    assert(strstr(got, "> 12 00 00 00 ") && strstr(got, "INQUIRY  in="));
    /* ... and its result, with the transport's own reason */
    assert(strstr(got, "trace #000001 < TRANSPORT FAILURE, no sense: ioctl:"));
    /* the issue line comes FIRST: it is what survives a command that hangs */
    assert(strstr(got, "trace #000001 +") < strstr(got, "trace #000001 <"));
    accudisc_close(dev);
}

static void test_an_untraced_handle_says_nothing_extra(void)
{
    accudisc_drive_id id;
    int err = 0;
    accudisc_device *dev = accudisc_open("/dev/null", 0, &err);

    assert(dev);
    got[0] = 0;
    lines = 0;
    accudisc_set_log(dev, sink, NULL);
    assert(accudisc_drive_identify(dev, &id) != ACCUDISC_OK);
    assert(!strstr(got, "trace") && "no trace without the flag");
    accudisc_close(dev);
}

/* TRACE_DATA implies TRACE: the control commands are still there. */
static void test_trace_data_implies_trace(void)
{
    accudisc_drive_id id;
    int err = 0;
    accudisc_device *dev =
        accudisc_open("/dev/null", ACCUDISC_OPEN_TRACE_DATA, &err);

    assert(dev);
    got[0] = 0;
    accudisc_set_log(dev, sink, NULL);
    assert(accudisc_drive_identify(dev, &id) != ACCUDISC_OK);
    assert(strstr(got, "INQUIRY"));
    accudisc_close(dev);
}

int main(void)
{
    test_the_opcode_table_and_the_bulk_rule();
    test_the_issue_line_names_the_command();
    test_a_CHECK_CONDITION_prints_its_sense_and_status();
    test_GOOD_TRANSPORT_and_resid();
    test_a_traced_handle_names_every_command_it_sends();
    test_an_untraced_handle_says_nothing_extra();
    test_trace_data_implies_trace();
    printf("ok test_trace\n");
    return 0;
}
