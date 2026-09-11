/* Per-command trace. Contract and rationale in trace.h. */

#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "internal.h"
#include "trace.h"

const char *adsc_op_name(uint8_t op)
{
    switch (op) {
    case 0x00: return "TEST UNIT READY";
    case 0x03: return "REQUEST SENSE";
    case 0x12: return "INQUIRY";
    case 0x15: return "MODE SELECT(6)";
    case 0x1A: return "MODE SENSE(6)";
    case 0x1B: return "START STOP UNIT";
    case 0x1E: return "PREVENT ALLOW MEDIUM REMOVAL";
    case 0x23: return "READ FORMAT CAPACITIES";
    case 0x25: return "READ CAPACITY";
    case 0x28: return "READ(10)";
    case 0x2A: return "WRITE(10)";
    case 0x35: return "SYNCHRONIZE CACHE";
    case 0x3B: return "WRITE BUFFER";
    case 0x3C: return "READ BUFFER";
    case 0x42: return "READ SUB-CHANNEL";
    case 0x43: return "READ TOC/PMA/ATIP";
    case 0x46: return "GET CONFIGURATION";
    case 0x4A: return "GET EVENT STATUS NOTIFICATION";
    case 0x51: return "READ DISC INFORMATION";
    case 0x52: return "READ TRACK INFORMATION";
    case 0x54: return "SEND OPC INFORMATION";
    case 0x55: return "MODE SELECT(10)";
    case 0x5A: return "MODE SENSE(10)";
    case 0x5B: return "CLOSE TRACK/SESSION";
    case 0x5C: return "READ BUFFER CAPACITY";
    case 0x5D: return "SEND CUE SHEET";
    case 0xA1: return "BLANK";
    case 0xA8: return "READ(12)";
    case 0xAA: return "WRITE(12)";
    case 0xAC: return "GET PERFORMANCE";
    case 0xB6: return "SET STREAMING";
    case 0xB9: return "READ CD MSF";
    case 0xBB: return "SET CD SPEED";
    case 0xBD: return "MECHANISM STATUS";
    case 0xBE: return "READ CD";
    default:   return op >= 0xC0 ? "vendor-specific" : "?";
    }
}

int adsc_op_is_bulk(uint8_t op)
{
    switch (op) {
    case 0x28: case 0xA8: case 0xB9: case 0xBE:   /* reads */
    case 0x2A: case 0xAA:                         /* writes */
        return 1;
    default:
        return 0;
    }
}

int adsc_trace_shows(int level, uint8_t op)
{
    if (level >= ADSC_TRACE_DATA)
        return 1;
    return level >= ADSC_TRACE_CTRL && !adsc_op_is_bulk(op);
}

void adsc_trace_hex(char *out, size_t cap, const uint8_t *p, size_t n)
{
    size_t used = 0;

    if (!out || cap == 0)
        return;
    out[0] = '\0';
    for (size_t i = 0; i < n && used + 4 <= cap; i++)
        used += (size_t)snprintf(out + used, cap - used, i ? " %02x" : "%02x",
                                 p[i]);
}

void adsc_trace_fmt_issue(char *out, size_t cap, uint32_t seq, double t_s,
                          const adsc_cmd *cmd)
{
    char cdb[ADSC_CDB_MAX * 3 + 1];
    char dir[32];

    adsc_trace_hex(cdb, sizeof cdb, cmd->cdb, cmd->cdb_len);
    switch (cmd->dir) {
    case ADSC_XFER_IN:  snprintf(dir, sizeof dir, "in=%u", cmd->buf_len);  break;
    case ADSC_XFER_OUT: snprintf(dir, sizeof dir, "out=%u", cmd->buf_len); break;
    default:            snprintf(dir, sizeof dir, "no-data");               break;
    }
    snprintf(out, cap, "trace #%06u +%.6f > %s  %s  %s timeout=%us", seq, t_s,
             cdb, adsc_op_name(cmd->cdb[0]), dir,
             (cmd->timeout_ms ? cmd->timeout_ms : ADSC_TIMEOUT_CTRL_MS) / 1000u);
}

void adsc_trace_fmt_result(char *out, size_t cap, uint32_t seq,
                           const adsc_cmd *cmd, int rc, double ms)
{
    char what[128];
    char resid[24] = "";

    if (rc == ACCUDISC_OK) {
        snprintf(what, sizeof what, "GOOD");
    } else if (rc == ACCUDISC_ERR_SENSE) {
        accudisc_sense s;

        adsc_sense_decode(cmd->sense, cmd->sense_len, &s);
        if (s.valid)
            snprintf(what, sizeof what,
                     "CHECK CONDITION %x/%02x/%02x (status=0x%02x host=0x%02x "
                     "driver=0x%02x)", s.key, s.asc, s.ascq, cmd->scsi_status,
                     cmd->host_status, cmd->driver_status);
        else
            snprintf(what, sizeof what,
                     "CHECK CONDITION, sense undecodable (status=0x%02x "
                     "host=0x%02x driver=0x%02x)", cmd->scsi_status,
                     cmd->host_status, cmd->driver_status);
    } else {
        char io[96];

        adsc_io_detail(cmd, io, sizeof io);
        snprintf(what, sizeof what, "TRANSPORT FAILURE, no sense: %s", io);
    }
    /* A residual is meaningless when the ioctl itself failed: nothing moved,
     * and SG_IO never reported a count. */
    if (!cmd->io_errno && cmd->resid)
        snprintf(resid, sizeof resid, " resid=%u", cmd->resid);
    snprintf(out, cap, "trace #%06u < %s  %.3f ms%s", seq, what, ms, resid);
}

uint64_t adsc_mono_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* Hex-dump a buffer in 32-byte rows, one log line each, at most `max` bytes. */
static void dump(struct accudisc_device *dev, uint32_t seq, const char *tag,
                 const uint8_t *p, uint32_t n, uint32_t max)
{
    char hex[32 * 3 + 1];
    uint32_t shown = n < max ? n : max;

    for (uint32_t off = 0; off < shown; off += 32) {
        uint32_t row = shown - off < 32 ? shown - off : 32;

        adsc_trace_hex(hex, sizeof hex, p + off, row);
        adsc_dev_log(dev, "trace #%06u   %s %04x: %s", seq, tag, off, hex);
    }
    if (n > shown)
        adsc_dev_log(dev, "trace #%06u   %s ... %u more bytes not shown", seq,
                     tag, n - shown);
}

static void issue(struct accudisc_device *dev, const adsc_cmd *cmd,
                  uint32_t seq, uint64_t t0)
{
    char line[256];

    adsc_trace_fmt_issue(line, sizeof line, seq,
                         (double)(t0 - dev->trace_t0_ns) / 1e9, cmd);
    adsc_dev_log(dev, "%s", line);
    if (cmd->dir == ADSC_XFER_OUT && cmd->buf && !adsc_op_is_bulk(cmd->cdb[0]))
        dump(dev, seq, "out", cmd->buf, cmd->buf_len, ADSC_TRACE_OUT_MAX);
}

void adsc_trace_before(struct accudisc_device *dev, adsc_cmd *cmd,
                       uint32_t *seq, uint64_t *t0, int *shown)
{
    /* The sequence number advances for EVERY command, traced or not, so a
     * gap between two numbers in the log counts the bulk transfers that went
     * unshown between them. */
    *seq = ++dev->trace_seq;
    *t0 = adsc_mono_ns();
    *shown = adsc_trace_shows(dev->trace, cmd->cdb[0]);
    /* Printed BEFORE the command is issued, so a command that never returns —
     * the USB bridge wedging with the process in D state — is still named. */
    if (*shown)
        issue(dev, cmd, *seq, *t0);
}

void adsc_trace_after(struct accudisc_device *dev, const adsc_cmd *cmd,
                      int rc, uint32_t seq, uint64_t t0, int shown)
{
    char line[256];
    double ms = (double)(adsc_mono_ns() - t0) / 1e6;

    /* A failing bulk transfer is always worth a line, at any level. Its issue
     * line is printed late, but carries the time it was really issued. */
    if (!shown) {
        if (rc == ACCUDISC_OK)
            return;
        issue(dev, cmd, seq, t0);
    }
    adsc_trace_fmt_result(line, sizeof line, seq, cmd, rc, ms);
    adsc_dev_log(dev, "%s", line);
    if (rc != ACCUDISC_OK && cmd->sense_len)
        dump(dev, seq, "sense", cmd->sense, cmd->sense_len, ADSC_SENSE_LEN);
    if (rc == ACCUDISC_OK && cmd->dir == ADSC_XFER_IN && cmd->buf &&
        !adsc_op_is_bulk(cmd->cdb[0]))
        dump(dev, seq, "in ", cmd->buf, cmd->buf_len - cmd->resid,
             ADSC_TRACE_IN_MAX);
}

void adsc_dev_trace_note(struct accudisc_device *dev, const char *fmt, ...)
{
    char msg[224];
    va_list ap;

    if (!dev || !dev->trace)
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    adsc_dev_log(dev, "trace: -- %s", msg);
}
