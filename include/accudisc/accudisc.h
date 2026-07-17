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
#define ACCUDISC_VERSION_MINOR 1
#define ACCUDISC_VERSION_PATCH 0

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
    ACCUDISC_ERR_NOTFOUND    = -10 /* requested data legitimately absent
                                      (MCN/ISRC/CD-Text/driver/offset) —
                                      never a transport failure */
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

/* ATIP (Absolute Time In Pregroove) of a recordable disc: the lead-in start
 * time doubles as the manufacturer identification code (97:SS:FF for CD-R),
 * the lead-out last-possible start gives the disc capacity, and `erasable`
 * distinguishes CD-RW. `manufacturer` is looked up from the built-in ATIP
 * catalog (NULL if the code is not listed). All fields are reported raw as the
 * disc encodes them; AccuDisc does not judge them. */
typedef struct accudisc_atip {
    uint8_t lead_in_min, lead_in_sec, lead_in_frame;
    uint8_t lead_out_min, lead_out_sec, lead_out_frame;
    int         erasable;      /* 1 = CD-RW, 0 = CD-R, -1 = unknown */
    const char *manufacturer;  /* static string, or NULL if unlisted */
} accudisc_atip;

/* Read and decode the disc ATIP. Returns ACCUDISC_ERR_NOTFOUND when the drive
 * answers but the disc carries no ATIP (e.g. a pressed CD), distinct from
 * ACCUDISC_ERR_SENSE. Non-destructive (a read). */
ACCUDISC_API int accudisc_read_atip(accudisc_device *dev, accudisc_atip *out);

/* Look up a manufacturer name from an ATIP code directly (min:sec:frame).
 * Matches on min:sec (the manufacturer key); frame is a per-media variant.
 * Returns a static string or NULL. Pure function, no device needed. */
ACCUDISC_API const char *accudisc_atip_manufacturer(uint8_t min, uint8_t sec,
                                                    uint8_t frame);

/* ---- recording (DAO write) -------------------------------------------------
 * Burn one audio session Disc-At-Once. The caller supplies a cdrdao .toc and
 * the raw audio BIN it references; AccuDisc only moves the bits. Requires a
 * blank disc and an ACCUDISC_OPEN_RDWR handle. Provisional API — the write
 * engine is young; fields may grow. */
typedef struct accudisc_write_opts {
    int simulate;   /* test-write: run the full path with the laser off */
    int byteswap;   /* swap each 16-bit audio sample before writing */
    int speed;      /* 0 = leave the drive's current write speed */
} accudisc_write_opts;

/* Burn toc_path (a cdrdao .toc) + bin_path (the raw s16 audio it names).
 * progress (may be NULL) is called with sectors done / total. Returns
 * ACCUDISC_OK, ACCUDISC_ERR_UNSUPPORTED if the disc is not blank, or a
 * transport/parse error. */
ACCUDISC_API int accudisc_write(accudisc_device *dev, const char *toc_path,
                                const char *bin_path,
                                const accudisc_write_opts *opts,
                                void (*progress)(void *user, uint32_t done,
                                                 uint32_t total),
                                void *user);

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

/* ---- read-speed uncap (driver capability) ----------------------------------
 * Firmware caps CD read speed on some media; where the vendor offers an
 * override (Plextor: "SpeedRead"), this toggles it, raising the ceiling
 * reported by accudisc_get_speed (PX-716A: 40x -> 48x). Speed is still
 * commanded through accudisc_set_speed — this only lifts the limit.
 * ACCUDISC_ERR_UNSUPPORTED without an attached driver offering it.
 *
 * The setting is DRIVE state: it persists after the handle is closed, until
 * changed again or the drive is power-cycled. A caller that flips it for one
 * operation should read the prior value first and restore it. */
ACCUDISC_API int accudisc_speed_uncap_get(accudisc_device *dev, int *on);
ACCUDISC_API int accudisc_speed_uncap_set(accudisc_device *dev, int on);

/* Best-effort drive read-speed control, in Nx CD speed (176 kB/s units).
 * Prefers SET STREAMING (0xB6, a ceiling the drive enforces; needs
 * CAP_SYS_RAWIO), falling back to the unprivileged CDROM_SELECT_SPEED path. */
ACCUDISC_API int accudisc_set_speed(accudisc_device *dev, unsigned speed_x);

/* Mode page 2A max/current read speed in kB/s (divide by 176 for Nx). */
ACCUDISC_API int accudisc_get_speed(accudisc_device *dev,
                                    unsigned *max_kbps, unsigned *cur_kbps);

/* ---- drive rotation / nominal performance curve (GET PERFORMANCE 0xAC) -----
 * The read-speed curve the drive reports for the loaded medium, as a list of
 * {start_lba, start_kbps, end_lba, end_kbps} segments, and a classification of
 * its shape. Pure MMC and disc-independent on the drives tested (the curve is
 * RPM-derived); a drive that rejects the command yields count 0, which
 * classifies as ACCUDISC_ROTATION_UNKNOWN — the shape is never inferred. */
typedef enum {
    ACCUDISC_ROTATION_UNKNOWN = 0, /* command rejected / no descriptors */
    ACCUDISC_ROTATION_CLV,         /* constant linear velocity: one flat level */
    ACCUDISC_ROTATION_CAV,         /* constant angular velocity: rising rate */
    ACCUDISC_ROTATION_PCAV,        /* partial CAV: rises then caps flat */
    ACCUDISC_ROTATION_ZCLV,        /* zoned CLV: stepped flat levels */
} accudisc_rotation;

typedef struct accudisc_perf_desc {
    uint32_t start_lba;
    uint32_t start_kbps;
    uint32_t end_lba;
    uint32_t end_kbps;
} accudisc_perf_desc;

/* Fetch the nominal-performance curve. Writes up to max_out descriptors into
 * out and sets *count to the number returned (0 if the drive rejects GET
 * PERFORMANCE). Returns the command status; a rejection is not fatal — treat
 * it as "curve unknown". */
ACCUDISC_API int accudisc_get_performance(accudisc_device *dev,
                                          accudisc_perf_desc *out,
                                          uint32_t max_out, uint32_t *count);

/* Classify a performance curve's rotation strategy. Pure function over
 * drive-supplied descriptors (count 0 => UNKNOWN); no hardware access. */
ACCUDISC_API accudisc_rotation
accudisc_classify_rotation(const accudisc_perf_desc *desc, uint32_t count);

/* Spin the spindle down without ejecting (START STOP UNIT, straight to the
 * drive rather than through block-layer quirks). */
ACCUDISC_API int accudisc_spindle_stop(accudisc_device *dev);

/* Open the tray / unload the disc (START STOP UNIT, LoEj=1 Start=0). Straight
 * to the drive, so it works without a mounted block device. */
ACCUDISC_API int accudisc_eject(accudisc_device *dev);

/* Close the tray / load the disc (START STOP UNIT, LoEj=1 Start=1). A slot
 * loader with no disc may reject this; the drive's sense is returned. */
ACCUDISC_API int accudisc_load(accudisc_device *dev);

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
 * Returns ACCUDISC_ERR_NOTFOUND when the drive answers but the disc carries
 * no CD-Text; a drive that rejects format 5 outright still surfaces as
 * ACCUDISC_ERR_SENSE (deliberately not conflated with "absent"). */
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

/* Accurate Stream probe: does this drive read audio positionally
 * deterministically? Reads a span, then re-reads it from several different
 * starting LBAs (cache-defeated) and demands the overlapping sectors match
 * byte-for-byte. Probe a CLEAN disc area (damage reads as jitter).
 * *accurate = 1: positioning slips are largely prevented by the drive and
 * boundary overlap checking is near-redundant; 0: the drive can slip —
 * overlap checking is the only defence against the error class C2 is
 * structurally blind to. Factual drive capability: record it alongside the
 * read offset and C2 verdict. */
ACCUDISC_API int accudisc_probe_accurate_stream(accudisc_device *dev,
                                                uint32_t lba,
                                                uint8_t *accurate);

/* C2/audio alignment probe. Some drives return the C2 bitmap misaligned
 * with the audio bytes of the same sector by a small, constant, per-drive
 * amount (e.g. 2 sample pairs on the Plextor PX-716A). Anything consuming
 * fired bits as byte-exact damage positions (erasure feeds for parity
 * repair) must correct for it — misplaced erasures actively harm decoding.
 *
 * Sign convention: a fired bit at bitmap position i describes audio byte
 * i - 4*lag_pairs. Positive lag = the bitmap trails the audio.
 *
 * Method (no external reference needed): fired flags mark bytes the CIRC
 * decoder failed on, and failed bytes are unstable across cache-defeated
 * rereads — so flag positions are cross-correlated against reread
 * instability over candidate shifts; the agreement peak is the lag. The
 * probe scans [lba, lba+count) for C2-active sectors and rereads those, so
 * it needs DAMAGED media (and a span/speed where flags actually fire):
 * ACCUDISC_ERR_NOTFOUND = not enough C2/instability evidence to conclude
 * (clean disc, clean span, or flags incoherent with instability) — never
 * an I/O failure.
 *
 * REPORT-ONLY: AccuDisc never applies the lag to delivered bitmaps; it is
 * a factual drive property for the caller to record and apply. peak_milli
 * is agreement against a PROXY oracle (reread instability), which cannot
 * see bytes that fail identically in paired reads — expect it well below
 * a database oracle's precision. A verdict is only returned when the peak
 * dominates every other shift (3x contrast) on top of evidence floors, so
 * an OK result is already an unambiguous alignment. */
typedef struct accudisc_c2_lag {
    int32_t  lag_pairs;     /* the peak shift, in sample pairs (4 bytes) */
    uint32_t sectors_active;/* C2-active sectors seen in the scan pass */
    uint32_t flags_used;    /* fired C2 bits contributing at the peak */
    uint32_t diff_bytes;    /* unstable byte observations accumulated */
    uint16_t peak_milli;    /* flags landing on unstable bytes at the peak, ‰ */
    uint16_t runner_milli;  /* best agreement at any OTHER shift, ‰ */
} accudisc_c2_lag;
/* On ACCUDISC_ERR_NOTFOUND the struct is still filled with whatever
 * evidence was gathered (all-zero = no C2 fired in the span at all), so
 * callers can distinguish "clean span" from "C2 seen but inconclusive". */

ACCUDISC_API int accudisc_probe_c2_lag(accudisc_device *dev, uint32_t lba,
                                       uint32_t count, accudisc_c2_lag *out);

/* Achievable-speed-ladder probe. CDROM_SELECT_SPEED is best-effort and
 * mode page 2A reports the SETTING, not reality — the only ground truth
 * for what a rung delivers is a timed streaming read. For each candidate
 * speed this sets it, lets the drive settle with a warm-up read, then
 * times a streaming read (~1 second's worth of audio at the requested
 * speed) in a fresh window inside [lba, lba+count) — each rung gets its
 * own window so the drive cache can never serve a remeasure.
 *
 * Interpretation notes: measured_cx is achieved rate at THIS radius (CAV
 * drives read outer tracks faster — probe mid-disc for a representative
 * figure); rungs whose measured_cx collapse to the same value are
 * indistinguishable on this rig (bus or firmware limited) and one of them
 * suffices in a recovery ladder. The drive is LEFT at the last candidate
 * tested (speed is never auto-restored, as with reads). */
typedef struct accudisc_speed_rung {
    uint16_t requested_x;  /* the candidate passed in */
    uint16_t reported_x;   /* page 2A current speed after the set (0 = n/a) */
    uint16_t measured_cx;  /* timed streaming rate, centi-x (531 = 5.31x) */
} accudisc_speed_rung;

ACCUDISC_API int accudisc_probe_speed_ladder(accudisc_device *dev,
                                             uint32_t lba, uint32_t count,
                                             const uint16_t *candidates,
                                             uint8_t ncand,
                                             accudisc_speed_rung *out);

/* ---- status map ------------------------------------------------------------
 * The frame-accurate progress surface. The caller owns a buffer of one byte
 * per sector and passes it to a read (later: write) request; the engine
 * updates the byte for each sector with a single relaxed atomic store as its
 * state settles. Any thread — or, if the caller puts the buffer in shared
 * memory, any process — can poll it at zero syscall cost to draw progress
 * bars or EAC-style per-sector disc maps. No pipes, no events, no locks;
 * byte i is always the current best knowledge of sector (lba + i).
 *
 * Every state below is a RELATIVE claim — "stable/clean/unstable across the
 * reads of this run" — never verification against the pressing's canonical
 * bytes. A drive that misreads deterministically passes every relative
 * check; absolute gates (AccurateRip, CTDB) are the calling application's
 * job and always outrank anything recorded here.
 *
 * Byte layout: low nibble = state, high nibble = severity:
 *   C2        ~log2 of the sector's fired C2 bit count (1..15)
 *   RECOVERED number of extra reads it took (1..15)
 *   SUSPECT   ~log2 of the disagreeing byte count between reads
 *   others    0 */
#define ACCUDISC_MAP_PENDING   0x0 /* not yet attempted */
#define ACCUDISC_MAP_OK        0x1 /* read clean */
#define ACCUDISC_MAP_C2        0x2 /* delivered with fired C2 pointer(s) */
#define ACCUDISC_MAP_HARD      0x3 /* unreadable — zero-filled in the output */
#define ACCUDISC_MAP_RECOVERED 0x4 /* problem seen, clean/agreeing copy won */
#define ACCUDISC_MAP_SUSPECT   0x5 /* reads disagree — best-effort delivered */

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
    /* accuracy strategy (all off = single-pass fast read): */
    uint8_t c2_retries;    /* cache-defeated rereads hunting a C2-clean copy
                            * of each flagged sector (requires c2 != NONE);
                            * best read wins, whole sector replaced so
                            * AUDIO/C2/SUB stay single-read aligned */
    uint8_t verify_passes; /* >= 2: reread every chunk with cache defeat and
                            * compare audio; disagreeing sectors resolved by
                            * consensus (any two identical independent reads),
                            * else delivered best-effort as SUSPECT */
    uint8_t overlap_sectors; /* boundary overlap check: extend each chunk
                            * read by k trailing sectors and compare them
                            * against the next chunk's head — catches drive
                            * slips at chunk seams that back-to-back reads
                            * can't see. Mismatches go to consensus.
                            * 0 = off; clamped to 8 */
    /* speed ladder for problem-sector rereads: rescue/consensus attempt n
     * runs at ladder[min(n-1, len-1)] (e.g. {32,16,8,4} — descend toward
     * slow, careful reads). Pick rungs that differ from speed_x: consensus
     * votes must be speed-diverse, since a drive can misread the same way
     * at the same speed every time. (Verify passes themselves stream at
     * speed_x — drives recalibrate on every speed change, so per-chunk
     * speed switching thrashes; run whole-range passes at different
     * speed_x yourself for a full speed-diverse sweep.) The pass speed is
     * restored before the next streaming chunk. NULL/0 = reread at the
     * current speed. Caller-owned; must outlive the call. */
    const uint16_t *speed_ladder;
    uint8_t ladder_len;
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
    uint64_t rereads;         /* problem-driven extra sector reads issued */
    uint64_t sectors_recovered; /* problem seen, clean/agreeing copy won */
    uint64_t sectors_suspect;   /* consensus failed, best-effort delivered */
    uint64_t slips;           /* disagreements that were a pure positional
                               * shift (reads identical modulo offset) — the
                               * C2-invisible slip class; a nonzero count on
                               * a drive says: use overlap checking */
    /* Q-subchannel health, counted only for --sub raw reads over the sector
     * data actually delivered. The subchannel has no CIRC (C1/C2) protection —
     * a per-frame CRC-16 is its only integrity check, and it fails
     * independently of the audio C2 stats above. subq_bad = subq_total -
     * subq_ok is the pregap/index/MSF metadata lost on this pass. */
    uint64_t subq_total;      /* Q frames examined (delivered sectors w/ raw sub) */
    uint64_t subq_ok;         /* frames whose CRC-16 verified */
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

/* ---- index / pregap map ----------------------------------------------------
 * The TOC gives only index-1 (track start). Pregaps (index 0) and intra-track
 * indices live ONLY in the Q subchannel, which carries no CIRC — a per-frame
 * CRC-16 is its sole integrity check. This decodes a raw-subchannel scan into
 * a per-track index/pregap map, cross-referenced against the TOC's
 * authoritative index-1 boundaries, gating on CRC so damage cannot inject a
 * false index. It is purely observational: where the boundary approach is
 * damaged it reports UNKNOWN rather than guessing — model-based reconstruction
 * across the gap is a separate step. */

typedef enum {
    ACCUDISC_PREGAP_NO_DATA = 0, /* scan did not cover this boundary */
    ACCUDISC_PREGAP_NONE,        /* gapless: clean approach, no index-0 frames */
    ACCUDISC_PREGAP_PRESENT,     /* pregap observed; start/length reconstructed */
    ACCUDISC_PREGAP_UNKNOWN,     /* boundary damaged; presence indeterminate */
} accudisc_pregap_state;

typedef struct accudisc_index_map {
    uint8_t  track;         /* 1..99 */
    uint8_t  pregap_state;  /* accudisc_pregap_state */
    uint8_t  max_index;     /* highest CRC-good index seen for this track */
    int32_t  index1_lba;    /* authoritative track start (from the TOC) */
    int32_t  q_index1_lba;  /* index-1 start as seen in Q, or -1 (cross-check) */
    int32_t  index0_lba;    /* reconstructed pregap start, or -1 */
    uint32_t pregap_frames; /* index1_lba - index0_lba when PRESENT, else 0.
                             * A lower bound if the transition frame itself was
                             * CRC-bad (recovered exactly only by the model). */
    uint32_t crc_ok;        /* CRC-good position frames in the boundary window */
    uint32_t crc_bad;       /* CRC-bad frames in the boundary window */
} accudisc_index_map;

/* Decode a per-sector raw subchannel scan (count*96 bytes for sectors
 * [base_lba, base_lba+count)) into a per-track index/pregap map. Writes up to
 * max_out entries (one per track in toc), returns the number written. The scan
 * need not be whole-disc: only the neighbourhood of each track boundary must be
 * covered, else that track is reported ACCUDISC_PREGAP_NO_DATA. */
ACCUDISC_API uint32_t accudisc_index_map_decode(const uint8_t *raw,
                                                int32_t base_lba, uint32_t count,
                                                const accudisc_toc *toc,
                                                accudisc_index_map *out,
                                                uint32_t max_out);

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
