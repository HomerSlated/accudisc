/* Per-command trace (0.36.0): what went to the drive, what came back, and how
 * long it took, one command at a time.
 *
 * WHY. On 2026-09-10 disc 4a's re-burn failed with 3/02/00 somewhere between
 * two log lines, and the only way to say which command refused it was to read
 * the source and argue from what the sense code means. A burn is one pass over
 * a medium that cannot be re-run, so the command stream IS the record.
 *
 * Enabled per handle by ACCUDISC_OPEN_TRACE / ACCUDISC_OPEN_TRACE_DATA; the
 * lines go to the handle's log sink. Formatting is split out as pure functions
 * so tests can pin the lines without a drive. */

#ifndef ADSC_TRACE_H
#define ADSC_TRACE_H

#include <stddef.h>
#include <stdint.h>

#include "transport/transport.h"

struct accudisc_device;

/* Trace levels, from the open flags. */
#define ADSC_TRACE_OFF  0
#define ADSC_TRACE_CTRL 1 /* every command except successful bulk transfers */
#define ADSC_TRACE_DATA 2 /* every command */

/* Longest data-out parameter list dumped, and longest data-in response. Cue
 * sheets for 99 tracks run to 3200 bytes; the first 256 carry the lead-in,
 * the MCN and the first tracks, which is where a malformed sheet shows. */
#define ADSC_TRACE_OUT_MAX 256u
#define ADSC_TRACE_IN_MAX  64u

/* The MMC name of an opcode, or "vendor-specific" for 0xC0-0xFF, or "?".
 * Core only: vendor opcode names belong to their driver (vendor isolation). */
const char *adsc_op_name(uint8_t op);

/* 1 for the bulk data transfers (READ(10/12), READ CD, READ CD MSF,
 * WRITE(10/12)): thousands per disc, traced at ADSC_TRACE_CTRL only when they
 * fail. Everything else is a control command. */
int adsc_op_is_bulk(uint8_t op);

/* Whether a command is traced BEFORE it is issued at this level. A command
 * not shown here is still shown after the fact if it fails. */
int adsc_trace_shows(int level, uint8_t op);

/* Space-separated hex of p[0..n) into out; always NUL-terminated. */
void adsc_trace_hex(char *out, size_t cap, const uint8_t *p, size_t n);

/* "trace #SEQ +T > CDB  NAME  dir=LEN timeout=Ns" */
void adsc_trace_fmt_issue(char *out, size_t cap, uint32_t seq, double t_s,
                          const adsc_cmd *cmd);

/* "trace #SEQ < OUTCOME ... MS ms [resid=N]" — rc is what adsc_transport_exec
 * returned for cmd. */
void adsc_trace_fmt_result(char *out, size_t cap, uint32_t seq,
                           const adsc_cmd *cmd, int rc, double ms);

/* Monotonic nanoseconds (CLOCK_MONOTONIC). */
uint64_t adsc_mono_ns(void);

/* The exec-side hooks, called from adsc_dev_exec only. */
void adsc_trace_before(struct accudisc_device *dev, adsc_cmd *cmd,
                       uint32_t *seq, uint64_t *t0, int *shown);
void adsc_trace_after(struct accudisc_device *dev, const adsc_cmd *cmd,
                      int rc, uint32_t seq, uint64_t t0, int shown);

#endif /* ADSC_TRACE_H */
