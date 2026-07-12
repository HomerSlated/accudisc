/* AccuDisc recording (DAO write) engine — internal interface.
 *
 * Phase 1 (in progress): write-parameters setup + simulate scaffolding.
 * The public accudisc_write_* API will land once the engine is complete
 * (see docs/RECORDING_PLAN.md). Nothing here is part of the stable ABI yet.
 */
#ifndef ADSC_WRITE_H
#define ADSC_WRITE_H

#include <stdint.h>

struct accudisc_device;

/* Requested / observed write-parameters (mode page 0x05) state. */
struct adsc_write_params {
    uint8_t write_type;   /* observed only: 2 = SAO/DAO */
    int     simulate;     /* test-write (no laser) */
    int     burnproof;    /* buffer-underrun protection */
    int     cdtext;       /* raw+P-W 2448 blocks (CD-Text lead-in) */
};

/* Program the drive for DAO audio per *wp. Non-committal: configures the
 * drive only, does not touch the disc. */
int adsc_write_set_params(struct accudisc_device *dev,
                          const struct adsc_write_params *wp);

/* Read back the current write-parameters page for verification. */
int adsc_write_get_params(struct accudisc_device *dev,
                          struct adsc_write_params *out);

/* Disc state relevant to writing (from READ DISC INFORMATION). */
struct adsc_disc_info {
    int erasable;     /* 1 = CD-RW, 0 = CD-R */
    int status;       /* 0 = blank, 1 = appendable, 2 = complete, 3 = other */
    int first_track;
    int last_track;
    int sessions;
};

/* Read + decode disc status. A DAO burn wants status == 0 (blank). */
int adsc_write_read_disc_info(struct accudisc_device *dev,
                              struct adsc_disc_info *out);

#endif /* ADSC_WRITE_H */
