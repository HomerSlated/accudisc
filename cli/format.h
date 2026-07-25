#ifndef ADSC_CLI_FORMAT_H
#define ADSC_CLI_FORMAT_H

#include <stdio.h>

#include <accudisc/accudisc.h>

/* Machine-interface output formatters, split out of main.c so they can be
 * exercised without a drive.
 *
 * Every function here is PURE over its arguments: no device handle, no globals,
 * no I/O beyond the FILE * it is handed. That is the whole point — the strings
 * these emit are the contract documented in docs/reference/cli-machine-interface.md,
 * and until this split the only way to observe them was to put the right
 * physical disc in the tray. Now a captured lead-in blob drives the identical
 * code path (tests/test_cli_toc_format.c).
 *
 * Do NOT add an accudisc_device * parameter to anything in this file. The
 * temptation arises the moment a formatter wants accudisc_last_io() for a
 * diagnostic; that re-couples the output to hardware and silently removes the
 * test coverage. Diagnostics that need the device stay in main.c.
 */

/* `accudisc toc` stdout, exactly. Writes the track lines, the session lines,
 * the lead-out line and the trailing `source=…` status line.
 *
 * The stderr degrade diagnostic is deliberately NOT here: it needs
 * accudisc_last_io(dev). See the note above.
 */
void adsc_cli_fmt_toc(FILE *out, const accudisc_toc *toc,
                      const accudisc_toc_info *info);

#endif /* ADSC_CLI_FORMAT_H */
