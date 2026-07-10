/* SPDX-License-Identifier: MIT */
/* accudisc.h — public API for libaccudisc.
 *
 * This header is the ABI contract: the CLI and all language bindings are
 * built against it exclusively. Keep it C-only, self-contained, and free of
 * internal types.
 */

#ifndef ACCUDISC_H
#define ACCUDISC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ACCUDISC_VERSION_MAJOR 0
#define ACCUDISC_VERSION_MINOR 0
#define ACCUDISC_VERSION_PATCH 1

#if defined(_WIN32)
#  define ACCUDISC_API __declspec(dllexport)
#else
#  define ACCUDISC_API __attribute__((visibility("default")))
#endif

/* ---- version ------------------------------------------------------------- */

/* Version of the library actually linked (compare against the macros
 * above to detect header/library skew). */
ACCUDISC_API const char *accudisc_version_string(void);
ACCUDISC_API void accudisc_version(int *major, int *minor, int *patch);

/* Free any buffer the library allocated for the caller (raw TOC/CD-Text
 * dumps). Bindings must route through this, not their runtime's free. */
ACCUDISC_API void accudisc_free(void *p);

/* ---- CD-DA sizes ----------------------------------------------------------
 * One CD-DA sector = 1/75 s of audio = 2352 bytes. The optional per-sector
 * companions a drive can return alongside it in the same READ CD transfer: */
#define ACCUDISC_BYTES_AUDIO   2352 /* raw s16le PCM user data */
#define ACCUDISC_BYTES_C2      294  /* C2 pointers: 2352 bits, 1/byte, MSB-first */
#define ACCUDISC_BYTES_C2_BEB  296  /* C2 + block-error-bits variant */
#define ACCUDISC_BYTES_SUB_RAW 96   /* raw P-W subcode, interleaved */
#define ACCUDISC_BYTES_SUB_Q   16   /* formatted Q subchannel block */

/* ---- errors ---------------------------------------------------------------
 * All fallible functions return ACCUDISC_OK (0) or a negative accudisc_err.
 * ACCUDISC_ERR_SENSE means the drive itself rejected the command
 * (CHECK CONDITION) — the decoded sense is available via
 * accudisc_last_sense() on the device the call was made against. */
typedef enum accudisc_err {
    ACCUDISC_OK              = 0,
    ACCUDISC_ERR_INVAL       = -1, /* invalid argument */
    ACCUDISC_ERR_NOMEM       = -2, /* allocation failure */
    ACCUDISC_ERR_OPEN        = -3, /* device could not be opened */
    ACCUDISC_ERR_IO          = -4, /* transport/host/driver failure */
    ACCUDISC_ERR_SENSE       = -5, /* drive returned CHECK CONDITION */
    ACCUDISC_ERR_SHORT       = -6, /* response shorter than required */
    ACCUDISC_ERR_UNSUPPORTED = -7, /* not supported by drive or build */
    ACCUDISC_ERR_CANCELLED   = -8, /* stopped by cancel flag or sink */
    ACCUDISC_ERR_CRC         = -9, /* checksum failed (Q frame, CD-Text pack) */
    ACCUDISC_ERR_NOTFOUND    = -10 /* scanned data not present (MCN/ISRC) */
} accudisc_err;

/* Static human-readable name for an accudisc_err value. */
ACCUDISC_API const char *accudisc_strerror(int err);

/* Decoded SCSI sense from the most recent failed command on a device.
 * valid is 0 when the failure produced no usable sense data. */
typedef struct accudisc_sense {
    uint8_t valid;
    uint8_t key;  /* sense key, e.g. 3 = MEDIUM ERROR, 4 = HARDWARE ERROR */
    uint8_t asc;  /* additional sense code */
    uint8_t ascq; /* additional sense code qualifier */
} accudisc_sense;

/* ---- device ---------------------------------------------------------------
 * A handle to one optical drive. Handles are not thread-safe; use one per
 * thread or serialize externally. (Reading a status map while another thread
 * drives the device is safe and is the intended progress-tracking pattern.) */
typedef struct accudisc_device accudisc_device;

/* Open read-write. Required for vendor opcodes, MODE SELECT, and writing:
 * the kernel's unprivileged SG_IO command filter blocks those on read-only
 * fds. Plain reading should not set this (least privilege). */
#define ACCUDISC_OPEN_RDWR 0x1u

/* Open the drive at path (e.g. "/dev/sr0"). Returns NULL on failure; if err
 * is non-NULL it receives the accudisc_err cause. */
ACCUDISC_API accudisc_device *accudisc_open(const char *path, unsigned flags,
                                            int *err);
ACCUDISC_API void accudisc_close(accudisc_device *dev);

/* Sense from the most recent ACCUDISC_ERR_SENSE/_IO failure on dev. */
ACCUDISC_API void accudisc_last_sense(const accudisc_device *dev,
                                      accudisc_sense *out);

/* INQUIRY identification strings, space-trimmed and NUL-terminated. */
typedef struct accudisc_drive_id {
    char vendor[9];
    char product[17];
    char revision[5];
} accudisc_drive_id;

ACCUDISC_API int accudisc_drive_identify(accudisc_device *dev,
                                         accudisc_drive_id *out);

/* Manufacturing read offset in samples for the identified drive (positive:
 * the drive reads early), from the built-in offset table.
 * ACCUDISC_ERR_NOTFOUND when the model is unknown. */
ACCUDISC_API int accudisc_read_offset(accudisc_device *dev, int32_t *samples);

/* Optional log sink for library/driver diagnostics (default: discarded). */
ACCUDISC_API void accudisc_set_log(accudisc_device *dev,
                                   void (*fn)(void *user, const char *msg),
                                   void *user);

/* ---- vendor drivers ---------------------------------------------------------
 * All proprietary/hardware-specific features live in external driver .so
 * files (see accudisc/driver.h); the core library is pure MMC/SG. Calling
 * accudisc_driver_attach IS the application's permission grant — without it
 * a device never issues a vendor opcode. Order: identify -> locate driver
 * (explicit name, or match by drive ID) -> load -> selftest (read/set/
 * re-read real device state) -> attach; any failure leaves the device on
 * generic MMC/SG, fully usable.
 *
 * name: driver to request ("plextor"), or NULL to auto-match the drive.
 * dir:  driver directory, or NULL for $ACCUDISC_DRIVER_DIR, falling back to
 *       the installed default.
 * Returns ACCUDISC_OK (attached), ACCUDISC_ERR_NOTFOUND (no driver file /
 * no match — warn-only situation, device stays usable), or
 * ACCUDISC_ERR_UNSUPPORTED (driver found but selftest failed; not attached).
 * Vendor opcodes need the kernel's full SG_IO command set: open the device
 * with ACCUDISC_OPEN_RDWR or selftest will fail. */
ACCUDISC_API int accudisc_driver_attach(accudisc_device *dev,
                                        const char *name, const char *dir);
ACCUDISC_API void accudisc_driver_detach(accudisc_device *dev);

/* Human-readable access method for logging by the calling application:
 * "generic MMC" or "driver <name> (<description>)". Never NULL. */
ACCUDISC_API const char *accudisc_access_method(accudisc_device *dev);

/* ---- hardware error counters (driver capability) ---------------------------
 * C1/C2/CU error census counters as exposed by vendor firmware (e.g.
 * Plextor Q-Check). ACCUDISC_ERR_UNSUPPORTED without an attached driver
 * offering the capability. read returns the counts accumulated since the
 * previous read and resets the interval. */
typedef struct accudisc_counters {
    uint32_t c1; /* correctable at C1 stage */
    uint32_t c2; /* correctable at C2 stage */
    uint32_t cu; /* uncorrectable */
} accudisc_counters;

ACCUDISC_API int accudisc_counter_scan_begin(accudisc_device *dev);
ACCUDISC_API int accudisc_counter_scan_read(accudisc_device *dev,
                                            accudisc_counters *out);
ACCUDISC_API int accudisc_counter_scan_end(accudisc_device *dev);

/* Best-effort drive read-speed control, in Nx CD speed (176 kB/s units);
 * uses the unprivileged CDROM_SELECT_SPEED path. */
ACCUDISC_API int accudisc_set_speed(accudisc_device *dev, unsigned speed_x);

/* Mode page 2A max/current read speed in kB/s (divide by 176 for Nx). */
ACCUDISC_API int accudisc_get_speed(accudisc_device *dev,
                                    unsigned *max_kbps, unsigned *cur_kbps);

/* Spin the spindle down without ejecting (START STOP UNIT, straight to the
 * drive rather than through block-layer quirks). */
ACCUDISC_API int accudisc_spindle_stop(accudisc_device *dev);

/* ---- TOC ------------------------------------------------------------------ */

typedef struct accudisc_track {
    uint8_t number;    /* 1..99 */
    uint8_t adr_ctrl;  /* raw ADR (high nibble) / CTRL (low nibble) */
    uint32_t lba;      /* first sector */
    uint32_t sectors;  /* to next track start (last track: to lead-out) */
} accudisc_track;

#define ACCUDISC_TRACK_IS_AUDIO(t) (((t)->adr_ctrl & 0x04) == 0)

typedef struct accudisc_toc {
    uint8_t first_track;
    uint8_t last_track;
    uint8_t track_count;
    uint32_t leadout_lba;
    accudisc_track tracks[99];
} accudisc_toc;

/* READ TOC format 0, parsed. Requires a disc. */
ACCUDISC_API int accudisc_read_toc(accudisc_device *dev, accudisc_toc *out);

/* Raw full TOC (READ TOC format 2: session structure, undecoded) — *out is
 * library-allocated (accudisc_free), *len includes the 2-byte length field. */
ACCUDISC_API int accudisc_read_full_toc(accudisc_device *dev,
                                        uint8_t **out, uint32_t *len);

/* Raw CD-Text packs from the lead-in (READ TOC format 5, undecoded).
 * Returns ACCUDISC_ERR_SHORT or ACCUDISC_ERR_SENSE when the disc carries
 * no CD-Text. */
ACCUDISC_API int accudisc_read_cdtext(accudisc_device *dev,
                                      uint8_t **out, uint32_t *len);

/* ---- feature probe ---------------------------------------------------------
 * What the drive CLAIMS (GET CONFIGURATION, CD Read feature 0x1E) versus what
 * it DOES (functional smoke reads at LBA 0, so a disc must be loaded).
 * Drives are known to advertise C2 they don't honour and vice versa; the
 * verdict is conservative: only "claimed AND functional" earns SUPPORTED. */

typedef enum accudisc_c2_verdict {
    ACCUDISC_C2_UNSUPPORTED = 0, /* C2 read fails outright */
    ACCUDISC_C2_SUPPORTED   = 1, /* advertised and functional */
    ACCUDISC_C2_UNVERIFIED  = 2  /* reads succeed but not advertised — don't trust */
} accudisc_c2_verdict;

typedef struct accudisc_features {
    uint8_t feature_present; /* CD Read feature descriptor returned */
    uint8_t current;         /* feature active for the loaded medium */
    uint8_t dap;             /* claims DAP (digital audio play) */
    uint8_t c2_claimed;      /* claims C2 error pointers */
    uint8_t cdtext_claimed;  /* claims CD-Text */
    /* functional smoke reads (1 = data returned): */
    uint8_t ok_c2;
    uint8_t ok_sub_raw;
    uint8_t ok_sub_q;
    uint8_t ok_c2_sub_raw;
    uint8_t ok_c2_sub_q;
    uint8_t c2_verdict;      /* accudisc_c2_verdict */
} accudisc_features;

ACCUDISC_API int accudisc_probe_features(accudisc_device *dev,
                                         accudisc_features *out);

/* ---- status map ------------------------------------------------------------
 * The frame-accurate progress surface. The caller owns a buffer of one byte
 * per sector and passes it to a read (later: write) request; the engine
 * updates the byte for each sector with a single relaxed atomic store as its
 * state settles. Any thread — or, if the caller puts the buffer in shared
 * memory, any process — can poll it at zero syscall cost to draw progress
 * bars or EAC-style per-sector disc maps. No pipes, no events, no locks;
 * byte i is always the current best knowledge of sector (lba + i).
 *
 * Byte layout: low nibble = state, high nibble = severity (C2-flagged
 * sectors: ~log2 of the sector's C2 error bit count, 1..15; else 0). */
#define ACCUDISC_MAP_PENDING 0x0 /* not yet attempted */
#define ACCUDISC_MAP_OK      0x1 /* read clean */
#define ACCUDISC_MAP_C2      0x2 /* data returned, C2 pointer(s) fired */
#define ACCUDISC_MAP_HARD    0x3 /* unreadable — zero-filled in the output */

#define ACCUDISC_MAP_STATE(b)    ((uint8_t)(b) & 0x0f)
#define ACCUDISC_MAP_SEVERITY(b) ((uint8_t)(b) >> 4)

/* ---- reading ---------------------------------------------------------------
 * One commanded read: the caller says what (range), with what companions
 * (C2 / subchannel), and how (chunking, retries, speed); the engine streams
 * raw sectors to the sink and reports per-sector status via the map.
 * AUDIO, C2 and SUB for a sector always come from the same READ CD transfer
 * (single-read alignment — the property C2-guided recovery depends on). */

/* c2 field */
#define ACCUDISC_C2_NONE     0
#define ACCUDISC_C2_PTRS     1 /* 294-byte pointer bitmap */
#define ACCUDISC_C2_PTRS_BEB 2 /* 296-byte pointers + block-error bits */
/* sub field */
#define ACCUDISC_SUB_NONE 0
#define ACCUDISC_SUB_RAW  1 /* raw interleaved P-W, 96 B */
#define ACCUDISC_SUB_Q    2 /* drive-formatted Q, 16 B */

typedef struct accudisc_read_req {
    uint32_t lba;      /* first sector */
    uint32_t count;    /* sectors to read (> 0) */
    uint8_t c2;        /* ACCUDISC_C2_* */
    uint8_t sub;       /* ACCUDISC_SUB_* */
    uint8_t any_type;  /* 0: expected type CD-DA; 1: any (mixed-mode spans) */
    uint8_t retries;   /* per-sector attempts after a chunk fails; 0 = 2 */
    uint16_t chunk_sectors; /* per READ CD command; 0 = max under 64 KiB */
    uint16_t speed_x;  /* set read speed first; 0 = leave as-is */
    uint8_t *status_map;        /* count bytes, or NULL; see status map above */
    const volatile int *cancel; /* poll: nonzero aborts at the next chunk; or NULL */
} accudisc_read_req;

/* One delivered chunk. data holds nsec sectors, each sector_len bytes laid
 * out AUDIO (audio_len) + C2 (c2_len) + SUB (sub_len). Hard-unreadable
 * sectors arrive zero-filled with an all-ones C2 bitmap so the streams never
 * desync. The pointer is only valid during the call. */
typedef struct accudisc_chunk {
    uint32_t lba;
    uint32_t nsec;
    const uint8_t *data;
    uint32_t sector_len;
    uint32_t audio_len;
    uint32_t c2_len;
    uint32_t sub_len;
} accudisc_chunk;

/* Return 0 to continue; nonzero cancels the read (ACCUDISC_ERR_CANCELLED). */
typedef int (*accudisc_sink_fn)(void *user, const accudisc_chunk *chunk);

typedef struct accudisc_read_stats {
    uint64_t sectors_read;    /* returned by the drive (excludes zero-fills) */
    uint64_t sectors_flagged; /* >= 1 C2 bit set */
    uint64_t c2_bits;         /* total fired C2 bits (real reads only) */
    uint64_t hard_errors;     /* sectors zero-filled after retries */
    uint32_t max_bits_sector; /* worst single sector's C2 bit count */
    int64_t first_flagged_lba; /* -1 if none */
    int64_t last_flagged_lba;  /* -1 if none */
    uint64_t sense_medium;    /* hard failures: sense key 3 */
    uint64_t sense_hardware;  /* sense key 4 */
    uint64_t sense_other;     /* any other terminal sense */
} accudisc_read_stats;

/* Blocking. Streams req->count sectors from req->lba into sink (which may be
 * NULL to read for status/stats only). stats may be NULL. */
ACCUDISC_API int accudisc_read_cdda(accudisc_device *dev,
                                    const accudisc_read_req *req,
                                    accudisc_sink_fn sink, void *user,
                                    accudisc_read_stats *stats);

/* ---- MSF <-> LBA ----------------------------------------------------------
 * MSF as it appears on disc; LBA 0 == 00:02:00 (the 150-sector pregap). */
ACCUDISC_API int32_t accudisc_msf_to_lba(uint8_t m, uint8_t s, uint8_t f);
ACCUDISC_API void accudisc_lba_to_msf(int32_t lba, uint8_t *m, uint8_t *s,
                                      uint8_t *f);

/* ---- Q subchannel ----------------------------------------------------------
 * Pure decoders for the 96-byte raw interleaved P-W stream captured with
 * ACCUDISC_SUB_RAW (bit 6 of each byte is the Q channel). All BCD fields are
 * decoded to binary; CRC-16 (X.25) is verified before anything is trusted. */

/* Q ADR values */
#define ACCUDISC_Q_POSITION 1 /* track/index/relative + absolute MSF */
#define ACCUDISC_Q_MCN      2 /* media catalog number */
#define ACCUDISC_Q_ISRC     3 /* track ISRC */

typedef struct accudisc_q {
    uint8_t adr;      /* ACCUDISC_Q_* */
    uint8_t control;  /* CTRL nibble: bit 2 = data track, bit 0 = pre-emphasis */
    uint8_t crc_ok;
    /* adr 1 (position): */
    uint8_t tno;      /* track number (0 in lead-in) */
    uint8_t index;    /* 0 = pregap */
    uint8_t rel_m, rel_s, rel_f; /* within track */
    uint8_t abs_m, abs_s, abs_f; /* on disc */
    /* adr 2: */
    char mcn[14];     /* 13 digits, NUL-terminated */
    /* adr 3: */
    char isrc[13];    /* 12 chars, NUL-terminated */
} accudisc_q;

/* Extract the 12 Q bytes from one raw interleaved 96-byte subcode block. */
ACCUDISC_API void accudisc_sub_extract_q(const uint8_t raw[96], uint8_t q[12]);

/* Parse a 12-byte Q frame. Fields are filled best-effort either way;
 * returns ACCUDISC_ERR_CRC when the frame's CRC does not verify. */
ACCUDISC_API int accudisc_q_parse(const uint8_t q[12], accudisc_q *out);

/* Convenience: scan the disc's Q stream (raw subchannel reads starting at
 * lba) for an MCN / a track ISRC; ACCUDISC_ERR_NOTFOUND when the disc does
 * not carry one. For ISRC, start at the target track's first sector. */
ACCUDISC_API int accudisc_scan_mcn(accudisc_device *dev, uint32_t lba,
                                   char mcn[14]);
ACCUDISC_API int accudisc_scan_isrc(accudisc_device *dev, uint32_t lba,
                                    char isrc[13]);

/* ---- full TOC (session structure) ------------------------------------------
 * Parses the blob from accudisc_read_full_toc (READ TOC format 2): raw
 * lead-in entries per session. Points 0x01-0x63 are track starts (address in
 * pmin/psec/pframe); 0xA0 = first track (+ disc type in psec), 0xA1 = last
 * track, 0xA2 = lead-out start. MSF values are kept raw — use
 * accudisc_msf_to_lba. */

typedef struct accudisc_fulltoc_entry {
    uint8_t session;
    uint8_t adr_ctrl; /* ADR high nibble, CTRL low */
    uint8_t point;
    uint8_t min, sec, frame;    /* running time in lead-in */
    uint8_t pmin, psec, pframe; /* the entry's address / payload */
} accudisc_fulltoc_entry;

typedef struct accudisc_fulltoc {
    uint8_t first_session;
    uint8_t last_session;
    uint16_t entry_count;
    accudisc_fulltoc_entry entries[136]; /* 99 tracks + 3/session + slack */
} accudisc_fulltoc;

ACCUDISC_API int accudisc_fulltoc_parse(const uint8_t *raw, uint32_t len,
                                        accudisc_fulltoc *out);

/* ---- CD-Text ----------------------------------------------------------------
 * Decodes the blob from accudisc_read_cdtext (18-byte packs). v0 scope:
 * block 0 (first language), single-byte character packs, types title /
 * performer / songwriter / UPC-ISRC. Bytes are copied through verbatim —
 * no character-set conversion, the caller interprets (passes UTF-8-authored
 * discs through undamaged). Packs failing CRC are skipped. */

#define ACCUDISC_TEXT_MAX 160

typedef struct accudisc_cdtext_strings {
    char title[ACCUDISC_TEXT_MAX];
    char performer[ACCUDISC_TEXT_MAX];
    char songwriter[ACCUDISC_TEXT_MAX];
    char code[ACCUDISC_TEXT_MAX]; /* UPC (album) / ISRC (track), type 0x8E */
} accudisc_cdtext_strings;

typedef struct accudisc_cdtext {
    accudisc_cdtext_strings album;      /* pack track number 0 */
    accudisc_cdtext_strings track[100]; /* indexed by track number, 1..99 */
} accudisc_cdtext;

/* *out is library-allocated (accudisc_free). ACCUDISC_ERR_SHORT when the
 * blob holds no usable packs. */
ACCUDISC_API int accudisc_cdtext_decode(const uint8_t *raw, uint32_t len,
                                        accudisc_cdtext **out);

#ifdef __cplusplus
}
#endif

#endif /* ACCUDISC_H */
