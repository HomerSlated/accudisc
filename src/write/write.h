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

/* ------------------------------------------------------------------ */
/* DAO layout model + cue sheet (SEND CUE SHEET, 0x5D)                 */

struct adsc_write_track {
    int      audio;         /* 1 = audio (only audio supported now) */
    int      preemphasis;   /* 50/15us pre-emphasis flag */
    int      copy;          /* copy-permitted flag */
    char     isrc[13];      /* 12 ASCII chars + NUL; "" if none */
    uint32_t index1_lba;    /* absolute image LBA of index 1 (track start) */
    uint32_t pregap;        /* sectors of pre-gap before index 1 (0 = none) */
};

struct adsc_write_toc {
    char     mcn[14];       /* 13 ASCII digits + NUL; "" if none */
    int      ntracks;
    uint32_t leadout_lba;   /* absolute image LBA of the lead-out */
    struct adsc_write_track track[99];
};

/* Build the SEND CUE SHEET payload (8 bytes/entry) for the audio DAO layout in
 * *toc. Writes up to cap bytes into out, sets *out_len. Mirrors cdrdao's
 * createCueSheet. Returns ACCUDISC_ERR_SHORT if cap is too small. */
int adsc_cuesheet_build(const struct adsc_write_toc *toc, uint8_t *out,
                        uint32_t cap, uint32_t *out_len);

#endif /* ADSC_WRITE_H */
