/* AccuDisc recording (DAO write) engine — internal interface.
 *
 * Phases 1-2 complete and hardware-verified: audio DAO burns bit-exact
 * (see docs/reference/RECORDING_PLAN.md §9). Phase 3 in progress: full TOC +
 * CD-Text. The public accudisc_write() API (include/accudisc/accudisc.h) is
 * live but provisional — fields may still grow until the engine is complete.
 */
#ifndef ADSC_WRITE_H
#define ADSC_WRITE_H

#include <stddef.h>
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

/* ---- page 05 round trip ----------------------------------------------------
 * MODE SELECT the page exactly as a burn would, MODE SENSE it straight back,
 * and compare. A MODE SELECT that returns GOOD is not evidence that the drive
 * holds the page: firmware may ignore a field, and a bridge may deliver a data
 * phase that is not the one we sent. Three burns that completed cleanly and
 * read blank after a reload (2026-09-28, 2026-10-02) are what a silently
 * unapplied page -- Test Write still set -- would look like, and until this
 * existed nothing in the tree could tell that from a physical write failure.
 *
 * Touches the drive's registers only: no disc is needed, none is written.
 * Both commands share one open handle so that nothing else can change the page
 * between them. Do NOT assume the page resets on eject: on a PX-716A
 * (2026-10-02) the page found at open, after a burn and a tray cycle, was
 * already DAO with BURN-Proof on -- so it either survives the tray cycle or is
 * that drive's default, and which is not established. */
#define ADSC_WPARAMS_PAGE_MAX 56u

enum {
    ADSC_WPRT_STAGE_SELECT = 1, /* failed sensing or selecting the page */
    ADSC_WPRT_STAGE_READBACK,   /* selected; the read-back failed */
    ADSC_WPRT_STAGE_DONE        /* all three captures are valid */
};

struct adsc_wparams_roundtrip {
    /* The page only, from its page-code byte. `before` is what the drive held,
     * `sent` what went down the wire, `after` what it then reported. */
    uint8_t  before[ADSC_WPARAMS_PAGE_MAX];
    uint8_t  sent[ADSC_WPARAMS_PAGE_MAX];
    uint8_t  after[ADSC_WPARAMS_PAGE_MAX];
    uint32_t before_len, sent_len, after_len;
    uint32_t diff_bytes; /* bytes of `after` that differ from `sent` */
    uint8_t  stage;      /* ADSC_WPRT_STAGE_*: how far it got */
    uint8_t  page_ok;    /* the whole page read back as sent */
    uint8_t  fields_ok;  /* write type, Test Write, BUFE, multisession, data
                          * block type and session format read back as sent */
    uint8_t  changed;    /* `sent` differed from `before`. When 0 a match
                          * proves NOTHING: an applied select and a dropped
                          * one read back the same */
    uint8_t  ignored;    /* asked for a change, got `before` back unaltered */
};

/* Returns ACCUDISC_OK when the round trip COMPLETED -- the verdict is in *rt,
 * and a completed round trip with page_ok == 0 still returns OK. An error
 * means a command failed; rt->stage says which and the captures up to that
 * stage are valid. */
int adsc_write_params_roundtrip(struct accudisc_device *dev,
                                const struct adsc_write_params *wp,
                                struct adsc_wparams_roundtrip *rt);

/* ---- page 05 read-back inside a burn ---------------------------------------
 * The burn's two halves of the round trip, apart, because the burn sends SET CD
 * SPEED between them: select and keep what went down the wire, then -- later,
 * and before anything touches the disc -- ask the drive what it holds now.
 *
 * The comparison is against the page that was SENT, not against the request:
 * a drive that refuses data block type 3 is retried without it and the burn
 * goes on (with a warning), so the request and the wire can differ by design.
 *
 * Unlike the round trip above there is no `changed` verdict. The probe asks
 * whether a select is APPLIED, which a match cannot show when the page already
 * held the values. The burn asks whether the drive holds the burn's fields
 * NOW, and for that a match is an answer however it came about. */
struct adsc_wparams_check {
    uint8_t  held[ADSC_WPARAMS_PAGE_MAX]; /* what MODE SENSE returned */
    uint32_t held_len;
    uint32_t diff_bytes; /* bytes of `held` that differ from the sent page */
    uint8_t  fields_ok;  /* every field below reads back as sent */
    /* Which burn field differs; all 0 when fields_ok. */
    uint8_t  bad_write_type, bad_test_write, bad_bufe, bad_multisession,
             bad_block_type, bad_session_format;
};

/* adsc_write_set_params, keeping the page as sent (ADSC_WPARAMS_PAGE_MAX
 * bytes at `sent`). */
int adsc_write_set_params_sent(struct accudisc_device *dev,
                               const struct adsc_write_params *wp,
                               uint8_t *sent, uint32_t *sent_len);

/* One MODE SENSE of page 05, compared with `sent`. ACCUDISC_OK means the
 * command completed and the verdict is in *out; fields_ok == 0 still returns
 * OK. Touches the drive's registers only. */
int adsc_write_params_check(struct accudisc_device *dev, const uint8_t *sent,
                            uint32_t sent_len, struct adsc_wparams_check *out);

/* Put a captured page back (the probe's pop for its own push). `page` is a
 * capture from the struct above; the mode header is re-read from the drive. */
int adsc_write_params_restore(struct accudisc_device *dev,
                              const uint8_t *page, uint32_t page_len);

/* Disc state relevant to writing (from READ DISC INFORMATION). */
struct adsc_disc_info {
    int erasable;     /* 1 = CD-RW, 0 = CD-R */
    int status;       /* 0 = blank, 1 = appendable, 2 = complete, 3 = other */
    int last_session; /* byte 2 bits 3-2: 0 = empty, 1 = incomplete,
                       * 2 = reserved/damaged, 3 = complete. A separate field
                       * from `status`, and the two can disagree. */
    int first_track;
    int last_track;
    int sessions;
    /* Lead-in geometry, from bytes 17-19 (lead-in start MSF). Needed for the
     * CD-Text lead-in write and for the cue sheet's lead-in entry. Derivation
     * mirrors cdrdao GenericMMC.cc:414-432: a start at or after 80:00:00 gives
     * the real extent up to 100:00:00 (LBA 450000); anything else is not a
     * usable ATIP lead-in and falls back to one minute (4500 sectors). This is
     * a property of the BLANK, not of how much CD-Text there is. */
    uint8_t  leadin_m, leadin_s, leadin_f; /* lead-in start MSF */
    uint32_t leadin_len;                   /* sectors of lead-in to fill */
};

/* Read + decode disc status. A DAO burn wants status == 0 (blank). */
int adsc_write_read_disc_info(struct accudisc_device *dev,
                              struct adsc_disc_info *out);

/* Writable lead-in extent, in sectors, from a lead-in start MSF (READ DISC
 * INFORMATION bytes 17-19). A start in [80:00:00, 100:00:00) yields
 * 450000 - startLBA; anything else — including a drive returning zeros or
 * nonsense — yields the 4500-sector (1 minute) fallback rather than an
 * underflowing subtraction. Pure; exposed for testing. */
uint32_t adsc_leadin_len_from_msf(uint8_t m, uint8_t s, uint8_t f);

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
 * createCueSheet. Returns ACCUDISC_ERR_SHORT if cap is too small.
 *
 * `di` (may be NULL) supplies the lead-in start MSF. When the toc carries a
 * CD-Text blob AND di is non-NULL, the lead-in entry gets data form 0x41
 * (CD-DA with P-W sub-channels, main channel generated by the drive) and the
 * lead-in start MSF; otherwise data form 0 and MSF 0, as before. */
int adsc_cuesheet_build(const struct adsc_write_toc *toc,
                        const struct adsc_disc_info *di, uint8_t *out,
                        uint32_t cap, uint32_t *out_len);

/* Parse a cdrdao .toc file (NUL-terminated text) into the DAO layout model:
 * per-track FILE offset/length, START pre-gaps, ISRC, pre-emphasis/copy, and
 * the disc MCN. Audio tracks only. Computes each track's start_lba/index1_lba
 * and the lead-out. Returns ACCUDISC_ERR_INVAL on malformed input.
 *
 * `err` (may be NULL) receives "line N: <what>" on failure and is set empty on
 * entry. Worth the parameter because ACCUDISC_ERR_INVAL for a whole file names
 * neither the line, the field, nor what was expected — cdda2img lost time to
 * that (§117.2) and so did we, on the same rejected FILE line, the same day. */
int adsc_toc_parse_cue(const char *text, struct adsc_write_toc *out,
                       char *err, size_t errcap);

/* Load a .toc (and, if cdtext_path is non-NULL, a raw CD-Text blob) from disk
 * into the DAO model. Slurps both files, parses the .toc via adsc_toc_parse_cue,
 * validates the blob (adsc_cdtext_blob_validate — structural + three-way CRC,
 * repairing zero-CRC packs in place) and attaches it as *out's borrowed cdtext
 * pointer. Validation runs HERE, at intake, so a bad blob costs an error rather
 * than a blank. On success, *cdtext_buf owns the blob buffer (NULL when no
 * cdtext_path) and the CALLER must free it after the burn; on any error nothing
 * is left allocated. `info` (may be NULL) receives the validation result, e.g.
 * how many zero-CRC packs were regenerated. Device-free so it is unit-testable.
 * `err` (may be NULL) receives the parser's "line N: <what>" detail.
 * Returns a parse/IO/open/validation error otherwise. */
struct adsc_cdtext_info;
int adsc_write_load_model(const char *toc_path, const char *cdtext_path,
                          struct adsc_write_toc *out, uint8_t **cdtext_buf,
                          struct adsc_cdtext_info *info,
                          char *err, size_t errcap);

/* ------------------------------------------------------------------ */
/* DAO burn orchestration                                             */

struct adsc_burn_opts {
    int simulate;   /* test-write: run the whole path with the laser off */
    int byteswap;   /* swap each 16-bit audio sample before writing */
    int speed;      /* 0 = leave the drive's current speed */
    int burnproof;  /* ACCUDISC_BURNPROOF_* — AUTO (0) asks the drive */
    size_t fifo_bytes; /* 0 = no FIFO (the old synchronous path) */
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
