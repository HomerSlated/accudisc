/* AccuDisc recording (DAO write) engine — internal interface.
 *
 * Phases 1-2 complete and hardware-verified: audio DAO burns bit-exact
 * (see docs/reference/RECORDING_PLAN.md §9). Phase 3 in progress: full TOC +
 * CD-Text. The public accudisc_write() API (include/accudisc/accudisc.h) is
 * live but provisional — fields may still grow until the engine is complete.
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
    uint32_t index1_lba;    /* absolute image LBA of index 1 (start_lba+pregap) */
    uint32_t pregap;        /* sectors of pre-gap before index 1 (0 = none) */
    /* For the write loop: this track occupies `sectors` LBAs starting at
     * (index1_lba - pregap); its audio is read from the BIN at file_offset. */
    uint32_t sectors;       /* total sectors incl. pre-gap (FILE length) */
    uint64_t file_offset;   /* byte offset into the BIN for this track */
};

struct adsc_write_toc {
    char     mcn[14];       /* 13 ASCII digits + NUL; "" if none */
    int      ntracks;
    uint32_t leadout_lba;   /* absolute image LBA of the lead-out */
    struct adsc_write_track track[99];
    /* CD-Text pass-through (Phase 3, §11): the raw READ TOC format-0x05 blob to
     * lay into the lead-in verbatim, exactly as accudisc_read_cdtext emits it.
     * BORROWED — the buffer is owned by whoever built the model (see
     * adsc_write_load_model / accudisc_write), not by this struct. NULL/0 when
     * the caller supplied no CD-Text. Not decoded here; the burn path consumes
     * the bytes as-is. */
    const uint8_t *cdtext;
    uint32_t       cdtext_len;
};

/* SEND CUE SHEET worst case, matching adsc_cuesheet_build's emission: MCN (2)
 * + lead-in (1) + 99 tracks each carrying [ISRC (2) + pregap (1) + track (1)]
 * + lead-out (1) = 400 entries of 8 bytes. Size any cue buffer to this so a
 * legitimate fully-populated 99-track disc is never rejected as ERR_SHORT. */
#define ADSC_CUE_MAX_ENTRIES (2u + 1u + 99u * 4u + 1u) /* 400 */
#define ADSC_CUE_MAX_BYTES   (ADSC_CUE_MAX_ENTRIES * 8u) /* 3200 */

/* Build the SEND CUE SHEET payload (8 bytes/entry) for the audio DAO layout in
 * *toc. Writes up to cap bytes into out, sets *out_len. Mirrors cdrdao's
 * createCueSheet. Returns ACCUDISC_ERR_SHORT if cap is too small. */
int adsc_cuesheet_build(const struct adsc_write_toc *toc, uint8_t *out,
                        uint32_t cap, uint32_t *out_len);

/* Parse a cdrdao .toc file (NUL-terminated text) into the DAO layout model:
 * per-track FILE offset/length, START pre-gaps, ISRC, pre-emphasis/copy, and
 * the disc MCN. Audio tracks only. Computes each track's start_lba/index1_lba
 * and the lead-out. Returns ACCUDISC_ERR_INVAL on malformed input. */
int adsc_toc_parse_cue(const char *text, struct adsc_write_toc *out);

/* Load a .toc (and, if cdtext_path is non-NULL, a raw CD-Text blob) from disk
 * into the DAO model. Slurps both files, parses the .toc via adsc_toc_parse_cue,
 * and attaches the CD-Text blob as *out's borrowed cdtext pointer. On success,
 * *cdtext_buf owns the blob buffer (NULL when no cdtext_path) and the CALLER
 * must free it after the burn; on any error nothing is left allocated. Device-
 * free so it is unit-testable. Returns a parse/IO/open error otherwise. */
int adsc_write_load_model(const char *toc_path, const char *cdtext_path,
                          struct adsc_write_toc *out, uint8_t **cdtext_buf);

/* ------------------------------------------------------------------ */
/* DAO burn orchestration                                             */

struct adsc_burn_opts {
    int simulate;   /* test-write: run the whole path with the laser off */
    int byteswap;   /* swap each 16-bit audio sample before writing */
    int speed;      /* 0 = leave the drive's current speed */
};

typedef void (*adsc_burn_progress)(void *user, uint32_t done, uint32_t total);

/* Burn one audio session Disc-At-Once: set write parameters, verify the disc
 * is blank, SEND CUE SHEET, write the lead-in gap + all track audio from
 * bin_fd (per-track file_offset), then SYNCHRONIZE CACHE. Progress via cb
 * (may be NULL). Returns ACCUDISC_OK on success. */
int adsc_write_run(struct accudisc_device *dev,
                   const struct adsc_write_toc *toc, int bin_fd,
                   const struct adsc_burn_opts *opts,
                   adsc_burn_progress cb, void *user);

#endif /* ADSC_WRITE_H */
