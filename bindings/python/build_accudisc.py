"""cffi builder for the AccuDisc Python binding.

API mode (``set_source`` + a real ``#include``), never ABI mode. The reason is
the one API_PLAN §7.1 gives for the ``size`` field: a binding that transcribes
struct layouts by hand produces well-formed numbers about the wrong bytes, and
nothing downstream can tell. Here the C compiler resolves every offset and
``sizeof`` from ``accudisc.h`` itself — the ``...;`` in each struct body means
"ask the compiler", so the declarations below are a statement of *which* fields
this binding touches, not a claim about where they live.

Header/library discovery, in order:

1. ``ACCUDISC_INCLUDE_DIR`` / ``ACCUDISC_LIB_DIR`` environment variables
2. ``pkg-config accudisc`` (an installed library)
3. the repo checkout this file sits in — ``include/`` and ``build/src/``

(3) is the development default: it also sets an RPATH at (3), so the extension
finds ``libaccudisc.so.0`` without ``LD_LIBRARY_PATH``.

RUNTIME PATH IS SEPARATELY SETTABLE, and that separation is the whole of the
relocatability fix. Until 0.3.0 the link-time and run-time directories were the
same list, so an extension built here recorded a RUNPATH into
``<repo>/build/src`` — correct on this machine, and silently wrong the moment
the package was installed anywhere, because the build tree it points at is not
part of the install. ``pip install .`` "worked" only because the build tree
happened to still be sitting where it was left.

``ACCUDISC_RUNTIME_LIB_DIR`` overrides it:

* unset            — runtime dirs follow the link dirs (development default)
* set to a path    — record exactly that RUNPATH (the *installed* libdir)
* set and **empty** — record NO RUNPATH, and let ``ld.so`` resolve the
  library from its ordinary search path. This is the right answer for a
  ``/usr`` install, and it is why the empty value has to be honoured rather
  than treated as "unset": ``os.environ.get`` cannot tell those apart, so
  membership is tested instead.

Under CMake this is set for you; see ``ACCUDISC_INSTALL_PYTHON``.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

from cffi import FFI

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parent.parent


def _pkg_config(*args: str) -> list[str]:
    exe = shutil.which("pkg-config")
    if not exe:
        return []
    try:
        out = subprocess.run(
            [exe, *args, "accudisc"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        return []
    return out.split()


def _runtime_dirs(link_dirs: list[str]) -> list[str]:
    """RUNPATH to record. See the module docstring for why empty is meaningful.

    Membership, not truthiness: an empty ACCUDISC_RUNTIME_LIB_DIR is an
    instruction ("record no RUNPATH"), not an absence.
    """
    if "ACCUDISC_RUNTIME_LIB_DIR" not in os.environ:
        return link_dirs
    val = os.environ["ACCUDISC_RUNTIME_LIB_DIR"].strip()
    return [val] if val else []


def _locate() -> tuple[list[str], list[str], list[str]]:
    """Return (include_dirs, library_dirs, runtime_library_dirs)."""
    inc = os.environ.get("ACCUDISC_INCLUDE_DIR")
    lib = os.environ.get("ACCUDISC_LIB_DIR")
    if inc and lib:
        return [inc], [lib], _runtime_dirs([lib])

    pc_inc = [a[2:] for a in _pkg_config("--cflags-only-I") if a.startswith("-I")]
    pc_lib = [a[2:] for a in _pkg_config("--libs-only-L") if a.startswith("-L")]
    if pc_inc and pc_lib:
        return pc_inc, pc_lib, _runtime_dirs(pc_lib)

    # Uninstalled: build against this checkout. DEVELOPMENT ONLY.
    #
    # This branch is why an installer can silently link the wrong library.
    # pkg-config is tried first and wins when it answers — but when it does NOT
    # (no accudisc.pc on the search path, which is the normal state on a
    # developer box that has never run `make install`), we fall through to here
    # and produce a working extension bound to a build tree. It behaves
    # identically until that tree moves, and nothing in the install output says
    # which library was chosen. cdda2img hit exactly this with `pipx inject`
    # (§123.2): the install succeeded without PKG_CONFIG_PATH, and the artefact
    # was the dev-tree one wearing an installed package's clothes.
    #
    # So: announce it, every time, and let an installer forbid it outright.
    if os.environ.get("ACCUDISC_REQUIRE_INSTALLED"):
        raise SystemExit(
            "accudisc: ACCUDISC_REQUIRE_INSTALLED is set and `pkg-config "
            "accudisc` did not answer, so the only remaining source is this "
            "checkout's build tree — which is what that variable exists to "
            "refuse. Install the library, or put its .pc on PKG_CONFIG_PATH:\n"
            "    PKG_CONFIG_PATH=<prefix>/lib64/pkgconfig pip install ...\n"
            "Unset ACCUDISC_REQUIRE_INSTALLED to build against the checkout."
        )
    tree_inc = _REPO / "include"
    tree_lib = _REPO / "build" / "src"
    if not (tree_inc / "accudisc" / "accudisc.h").is_file():
        raise SystemExit(
            f"accudisc.h not found under {tree_inc}. Set ACCUDISC_INCLUDE_DIR "
            f"and ACCUDISC_LIB_DIR, or install the library."
        )
    if not tree_lib.is_dir():
        raise SystemExit(
            f"{tree_lib} does not exist — build the C library first:\n"
            f"    cmake -B build && cmake --build build"
        )
    # Loud, on stderr, unconditionally. A silent fallback here is the whole
    # defect: the failure it causes appears later, somewhere else, as a missing
    # libaccudisc.so.0 that names nothing leading back to this choice.
    sys.stderr.write(
        f"accudisc: WARNING — no installed library found (pkg-config did not "
        f"answer), building against the DEVELOPMENT TREE at {tree_lib}.\n"
        f"accudisc:   The result is bound to that path and breaks if it moves. "
        f"Fine for development; NOT what you want in an installer.\n"
        f"accudisc:   For an installed library: "
        f"PKG_CONFIG_PATH=<prefix>/lib64/pkgconfig ...\n"
        f"accudisc:   To make this an error instead: ACCUDISC_REQUIRE_INSTALLED=1\n"
    )
    return [str(tree_inc)], [str(tree_lib)], _runtime_dirs([str(tree_lib)])


_INCLUDE_DIRS, _LIBRARY_DIRS, _RUNTIME_DIRS = _locate()

ffibuilder = FFI()

# ---------------------------------------------------------------------------
# Declarations.
#
# Every struct ends in `...;` so layout comes from the compiler. Every constant
# is `... ` for the same reason: cffi bakes in the value the preprocessor
# produced, so the numbers cannot drift from the header.
# ---------------------------------------------------------------------------
ffibuilder.cdef(
    r"""
/* ---- version ---------------------------------------------------------- */
#define ACCUDISC_VERSION_MAJOR ...
#define ACCUDISC_VERSION_MINOR ...
#define ACCUDISC_VERSION_PATCH ...

const char *accudisc_version_string(void);
void accudisc_version(int *major, int *minor, int *patch);
void accudisc_free(void *p);

/* ---- sizes ------------------------------------------------------------- */
#define ACCUDISC_BYTES_AUDIO ...
#define ACCUDISC_BYTES_C2 ...
#define ACCUDISC_BYTES_C2_BEB ...
#define ACCUDISC_BYTES_SUB_RAW ...
#define ACCUDISC_BYTES_SUB_Q ...

/* ---- errors ------------------------------------------------------------ */
typedef enum accudisc_err {
    ACCUDISC_OK,
    ACCUDISC_ERR_INVAL,
    ACCUDISC_ERR_NOMEM,
    ACCUDISC_ERR_OPEN,
    ACCUDISC_ERR_IO,
    ACCUDISC_ERR_SENSE,
    ACCUDISC_ERR_SHORT,
    ACCUDISC_ERR_UNSUPPORTED,
    ACCUDISC_ERR_CANCELLED,
    ACCUDISC_ERR_CRC,
    ACCUDISC_ERR_NOTFOUND,
    /* -11 (ACCUDISC_ERR_UNSAFE_COMBINATION) was removed in 0.6.0 and the value
     * is retired. Absent here deliberately: cffi verifies each named constant
     * against the real header at import, so leaving it would fail the build
     * rather than quietly produce a dead attribute. */
    ACCUDISC_ERR_ABI,
    ACCUDISC_ERR_NOT_BLANK,
    ACCUDISC_ERR_AMBIGUOUS,
    ...
} accudisc_err;

const char *accudisc_strerror(int err);

typedef struct accudisc_sense {
    uint8_t valid;
    uint8_t key;
    uint8_t asc;
    uint8_t ascq;
    ...;
} accudisc_sense;

/* ---- device ------------------------------------------------------------ */
#define ACCUDISC_OPEN_RDWR ...

typedef struct accudisc_device accudisc_device;

accudisc_device *accudisc_open(const char *path, unsigned flags, int *err);
void accudisc_close(accudisc_device *dev);
void accudisc_last_sense(const accudisc_device *dev, accudisc_sense *out);
const char *accudisc_last_io(accudisc_device *dev);

typedef struct accudisc_drive_id {
    char vendor[9];
    char product[17];
    char revision[5];
    ...;
} accudisc_drive_id;

int accudisc_drive_identify(accudisc_device *dev, accudisc_drive_id *out);
int accudisc_read_offset(accudisc_device *dev, int32_t *samples);

#define ACCUDISC_OFFSET_SRC_REDUMP ...
#define ACCUDISC_OFFSET_SRC_AR ...
#define ACCUDISC_OFFSET_F_CONFLICT ...
#define ACCUDISC_OFFSET_F_TRUNCATED ...
#define ACCUDISC_OFFSET_F_ADJUDICATED ...
#define ACCUDISC_OFFSET_F_GENERIC ...
#define ACCUDISC_OFFSET_NONE ...
#define ACCUDISC_OFFSET_MAX_VALUES ...

typedef struct accudisc_offset_info {
    uint32_t size;
    int32_t  read_offset;
    uint16_t ar_submissions;
    uint8_t  ar_agree_pct;
    uint8_t  sources;
    uint8_t  flags;
    uint8_t  n_values;
    int32_t  values[4];
    uint8_t  value_sources[4];
    uint32_t ar_acc_ok;
    uint32_t ar_acc_bad;
    ...;
} accudisc_offset_info;

int accudisc_offset_for_inquiry(const char *vendor, const char *product,
                                accudisc_offset_info *out);
int accudisc_offset_for_device(accudisc_device *dev, accudisc_offset_info *out);

/* ---- write offset (measurement, not a lookup) --------------------------- */
#define ACCUDISC_WOFF_SAMPLES ...
#define ACCUDISC_WOFF_PULSE_A ...
#define ACCUDISC_WOFF_PULSE_B ...
#define ACCUDISC_WOFF_PULSE_LEN ...
#define ACCUDISC_WOFF_SEARCH ...
#define ACCUDISC_WOFF_F_INCONSISTENT ...

typedef struct accudisc_write_offset_info {
    uint32_t size;
    int32_t  write_offset;
    int32_t  offset_a;
    int32_t  offset_b;
    int32_t  found_a;
    int32_t  found_b;
    uint8_t  flags;
    ...;
} accudisc_write_offset_info;

int accudisc_write_offset_signal(int16_t *pcm, uint32_t samples);
int accudisc_write_offset_locate(const int16_t *pcm, uint32_t samples,
                                 int32_t read_offset,
                                 accudisc_write_offset_info *out);

int accudisc_driver_attach(accudisc_device *dev, const char *name,
                           const char *dir);
void accudisc_driver_detach(accudisc_device *dev);
const char *accudisc_access_method(accudisc_device *dev);

int accudisc_set_speed(accudisc_device *dev, unsigned speed_x);
/* NOTE the order and the units: max first, current second, both kB/s. */
int accudisc_get_speed(accudisc_device *dev, unsigned *max_kbps,
                       unsigned *cur_kbps);
int accudisc_eject(accudisc_device *dev);
int accudisc_load(accudisc_device *dev);
int accudisc_spindle_stop(accudisc_device *dev);
int accudisc_read_cdtext(accudisc_device *dev, uint8_t **out, uint32_t *len);

void accudisc_set_log(accudisc_device *dev,
                      void (*fn)(void *user, const char *msg),
                      void *user);

/* ---- recording (DAO write) --------------------------------------------- */
#define ACCUDISC_WROTE_WITH_CAVEATS ...
#define ACCUDISC_BURNPROOF_AUTO ...
#define ACCUDISC_BURNPROOF_OFF ...
#define ACCUDISC_BURNPROOF_ON ...
#define ACCUDISC_FIFO_NONE ...
#define ACCUDISC_FIFO_MAX_BYTES ...
#define ACCUDISC_BUFFER_NONE ...
uint32_t accudisc_fifo_bytes_for(double seconds, unsigned speed_x);

/* IN struct, guarded by `size` since 0.3.0 — the library reads past the end of
 * a shorter one otherwise, and this is the only non-idempotent entry point in
 * the API. `size` is set from ffi.sizeof() at every call site. */
typedef struct accudisc_write_opts {
    uint32_t size;
    int simulate;
    int byteswap;
    int speed;
    const char *cdtext_path;
    int burnproof;
    uint32_t fifo_bytes;
    ...;
} accudisc_write_opts;

int accudisc_write(accudisc_device *dev, const char *toc_path,
                   const char *bin_path, const accudisc_write_opts *opts,
                   void (*progress)(void *user, uint32_t done, uint32_t total),
                   void *user);

/* ---- TOC --------------------------------------------------------------- */
typedef struct accudisc_track {
    uint8_t number;
    uint8_t adr_ctrl;
    uint8_t session;
    uint32_t lba;
    uint32_t sectors;
    uint32_t pregap;
    ...;
} accudisc_track;

typedef struct accudisc_session {
    uint8_t number;
    uint8_t first_track;
    uint8_t last_track;
    uint8_t audio_tracks;
    uint8_t data_tracks;
    uint32_t leadout_lba;
    ...;
} accudisc_session;

typedef struct accudisc_toc {
    uint8_t first_track;
    uint8_t last_track;
    uint8_t track_count;
    uint32_t leadout_lba;
    accudisc_track tracks[99];
    uint8_t session_count;
    accudisc_session sessions[99];
    uint8_t sessions_total;
    uint16_t anomalies;
    ...;
} accudisc_toc;

#define ACCUDISC_TOC_ANOM_LBA_ORDER ...
#define ACCUDISC_TOC_ANOM_OVERLAP ...
#define ACCUDISC_TOC_ANOM_LEADOUT_BEFORE ...
#define ACCUDISC_TOC_ANOM_PAST_LEADOUT ...
#define ACCUDISC_TOC_ANOM_EMPTY_TRACK ...
#define ACCUDISC_TOC_ANOM_NEGATIVE_LBA ...
#define ACCUDISC_TOC_ANOM_BAD_TRACK_NUM ...
#define ACCUDISC_TOC_ANOM_RANGE_MISMATCH ...
#define ACCUDISC_TOC_ANOM_BAD_SESSION ...
#define ACCUDISC_TOC_ANOM_UNTRUSTED_GEOMETRY ...

const char *accudisc_toc_anomaly_str(unsigned bit);
int accudisc_read_toc(accudisc_device *dev, accudisc_toc *out);
int accudisc_read_full_toc(accudisc_device *dev, uint8_t **out, uint32_t *len);

typedef struct accudisc_toc_info {
    uint8_t source;
    uint8_t degrade;
    int32_t degrade_err;
    uint8_t first_session;
    uint8_t last_session;
    uint8_t session_count;
    uint8_t disc_type;
    ...;
} accudisc_toc_info;

int accudisc_read_toc_src(accudisc_device *dev, accudisc_toc *out,
                          accudisc_toc_info *info);
const char *accudisc_toc_source_str(unsigned source);
const char *accudisc_toc_degrade_str(unsigned degrade);

int accudisc_toc_default_audio_session(const accudisc_toc *toc);
int accudisc_toc_session_range(const accudisc_toc *toc, uint8_t session,
                               uint32_t *lba, uint32_t *count);
int accudisc_toc_session_audio_range(const accudisc_toc *toc, uint8_t session,
                                     uint32_t *lba, uint32_t *count);
int accudisc_toc_track_range(const accudisc_toc *toc, uint8_t first,
                             uint8_t last, uint32_t *lba, uint32_t *count);

typedef struct accudisc_range_check {
    uint8_t ok;
    uint8_t reason;
    uint8_t session;
    uint8_t track;
    uint32_t first_bad_lba;
    ...;
} accudisc_range_check;

int accudisc_check_audio_range(const accudisc_toc *toc, uint32_t lba,
                               uint32_t count, accudisc_range_check *out);
const char *accudisc_range_reason_str(unsigned reason);

/* ---- CTDB parity repair -------------------------------------------------- */
#define ACCUDISC_CTDB_UNVERIFIED ...

typedef struct accudisc_ctdb_req {
    uint32_t size;
    uint32_t npar;
    uint32_t wire_stride;
    uint32_t image_first_frame;
    uint32_t image_frames;
    int32_t  offset_pairs;
    const uint8_t *pcm;
    uint64_t       pcm_bytes;
    const uint8_t *parity;
    uint64_t       parity_bytes;
    const uint8_t *pcm_erasures;
    uint64_t       pcm_erasures_bytes;
    ...;
} accudisc_ctdb_req;

typedef struct accudisc_ctdb_report {
    uint32_t size;
    int32_t  offset_pairs;
    uint32_t dirty_columns;
    uint32_t repaired_columns;
    uint32_t refused_columns;
    uint32_t erasure_columns;
    uint32_t corrections;
    uint32_t crc32_before;
    uint32_t crc32_after;
    uint32_t unverified_columns;
    ...;
} accudisc_ctdb_report;

int accudisc_ctdb_repair(const accudisc_ctdb_req *req, uint8_t *out_pcm,
                         accudisc_ctdb_report *report);

/* ---- disc classification ------------------------------------------------ */
typedef enum accudisc_disc_kind {
    ACCUDISC_DISC_NEITHER,
    ACCUDISC_DISC_BLANK,
    ACCUDISC_DISC_AUDIO,
    ...
} accudisc_disc_kind;

typedef enum accudisc_disc_reason {
    ACCUDISC_DISC_WHY_AUDIO,
    ACCUDISC_DISC_WHY_BLANK,
    ACCUDISC_DISC_WHY_DATA_CD,
    ACCUDISC_DISC_WHY_CLOSED_DATA,
    ACCUDISC_DISC_WHY_APPENDABLE,
    ACCUDISC_DISC_WHY_NO_MEDIUM,
    ACCUDISC_DISC_WHY_NOT_CD_PROFILE,
    ACCUDISC_DISC_WHY_UNREADABLE,
    ...
} accudisc_disc_reason;

typedef enum accudisc_tray_state {
    ACCUDISC_TRAY_UNKNOWN,
    ACCUDISC_TRAY_CLOSED,
    ACCUDISC_TRAY_OPEN,
    ...
} accudisc_tray_state;

#define ACCUDISC_DISC_STATUS_UNKNOWN ...

/* An OUT struct with no `size` field, like accudisc_c2_lag above. */
typedef struct accudisc_disc_probe {
    uint16_t profile;
    uint8_t erasable;
    uint8_t disc_status;
    uint8_t audio_tracks;
    uint8_t data_tracks;
    uint8_t kind;
    uint8_t reason;
    uint8_t tray;
    ...;
} accudisc_disc_probe;

int accudisc_probe_disc(accudisc_device *dev, accudisc_disc_probe *out);
const char *accudisc_disc_kind_str(unsigned kind);
const char *accudisc_disc_reason_str(unsigned reason);
const char *accudisc_tray_state_str(unsigned tray);

/* ---- feature probe ----------------------------------------------------- */
typedef enum accudisc_c2_verdict {
    ACCUDISC_C2_UNSUPPORTED,
    ACCUDISC_C2_SUPPORTED,
    ACCUDISC_C2_UNVERIFIED,
    ...
} accudisc_c2_verdict;

typedef struct accudisc_features {
    uint8_t feature_present;
    uint8_t current;
    uint8_t dap;
    uint8_t c2_claimed;
    uint8_t cdtext_claimed;
    uint8_t ok_c2;
    uint8_t ok_sub_raw;
    uint8_t ok_sub_q;
    uint8_t ok_c2_sub_raw;
    uint8_t ok_c2_sub_q;
    uint8_t c2_verdict;
    uint8_t mastering_present;
    uint8_t mastering_current;
    uint8_t buf_claimed;
    uint8_t sao_claimed;
    uint8_t test_write_claimed;
    ...;
} accudisc_features;

int accudisc_probe_features(accudisc_device *dev, accudisc_features *out);
int accudisc_probe_accurate_stream(accudisc_device *dev, uint32_t lba,
                                   uint8_t *accurate);

/* ---- C2/audio alignment ------------------------------------------------- */

/* An OUT struct with no `size` field, like accudisc_speed_rung above and for
 * the same reason. Unlike the rung there is only ever one of these, so a
 * stride mismatch cannot cascade — but a field mismatch still misreads every
 * number after the first, and lag_pairs is the one the caller acts on.
 * tests/test_binding.py pins sizeof(). */
typedef struct accudisc_c2_lag {
    int32_t  lag_pairs;
    uint32_t sectors_active;
    uint32_t flags_used;
    uint32_t diff_bytes;
    uint16_t peak_milli;
    uint16_t runner_milli;
    ...;
} accudisc_c2_lag;

int accudisc_probe_c2_lag(accudisc_device *dev, uint32_t lba, uint32_t count,
                          accudisc_c2_lag *out);

/* ---- achievable speed ladder ------------------------------------------- */
#define ACCUDISC_RUNG_UNKNOWN ...
#define ACCUDISC_RUNG_ADMITTED ...
#define ACCUDISC_RUNG_DUPLICATE ...
#define ACCUDISC_RUNG_QUANTIZED ...

/* An OUT ARRAY with no `size` field, deliberately — a per-element size would
 * mean trusting N separate caller claims, which is not what API_PLAN §7.1's
 * OUT rule says. The stride is therefore frozen: this binding is compiled
 * against the same header the library was, and tests/test_binding.py pins
 * sizeof() so a mismatch is a failed import rather than a garbled row 1. */
typedef struct accudisc_speed_rung {
    uint16_t requested_x;
    uint16_t reported_x;
    uint16_t measured_cx;
    uint16_t min_cx;
    uint16_t max_cx;
    uint16_t equiv_x;
    uint8_t  verdict;
    uint16_t band_cx[3];
    ...;
} accudisc_speed_rung;

int accudisc_probe_speed_ladder(accudisc_device *dev,
                                uint32_t lba, uint32_t count,
                                const uint16_t *candidates,
                                uint8_t ncand, uint8_t points,
                                accudisc_speed_rung *out);

/* ---- status map -------------------------------------------------------- */
#define ACCUDISC_MAP_PENDING ...
#define ACCUDISC_MAP_OK ...
#define ACCUDISC_MAP_C2 ...
#define ACCUDISC_MAP_HARD ...
#define ACCUDISC_MAP_RECOVERED ...
#define ACCUDISC_MAP_SUSPECT ...

/* ---- Q-subchannel health map ------------------------------------------- */
#define ACCUDISC_SUBQ_PENDING ...
#define ACCUDISC_SUBQ_OK ...
#define ACCUDISC_SUBQ_BAD ...
#define ACCUDISC_SUBQ_NO_POSITION ...
#define ACCUDISC_SUBQ_NO_AUDIO ...
#define ACCUDISC_SUBQ_MISPOSITION ...

/* ---- reading ----------------------------------------------------------- */
#define ACCUDISC_C2_NONE ...
#define ACCUDISC_C2_PTRS ...
#define ACCUDISC_C2_PTRS_BEB ...
#define ACCUDISC_SUB_NONE ...
#define ACCUDISC_SUB_RAW ...
#define ACCUDISC_SUB_Q ...

typedef struct accudisc_read_req {
    uint32_t size;
    uint32_t lba;
    uint32_t count;
    uint8_t c2;
    uint8_t sub;
    uint8_t any_type;
    uint8_t retries;
    uint16_t chunk_sectors;
    uint16_t speed_x;
    uint8_t c2_retries;
    uint8_t verify_passes;
    uint8_t overlap_sectors;
    const uint16_t *speed_ladder;
    uint8_t ladder_len;
    /* `uint8_t allow_unsafe` sat here until 0.6.0. It was padding, so its
     * removal moved nothing after it. */
    uint8_t *status_map;
    const volatile int *cancel;
    uint8_t *subq_map;
    uint32_t buffer_bytes;
    ...;
} accudisc_read_req;

typedef struct accudisc_chunk {
    uint32_t lba;
    uint32_t nsec;
    const uint8_t *data;
    uint32_t sector_len;
    uint32_t audio_len;
    uint32_t c2_len;
    uint32_t sub_len;
    ...;
} accudisc_chunk;

typedef int (*accudisc_sink_fn)(void *user, const accudisc_chunk *chunk);

typedef struct accudisc_read_stats {
    uint32_t size;
    uint64_t sectors_read;
    uint64_t sectors_flagged;
    uint64_t c2_bits;
    uint64_t hard_errors;
    uint32_t max_bits_sector;
    int64_t first_flagged_lba;
    int64_t last_flagged_lba;
    uint64_t sense_medium;
    uint64_t sense_hardware;
    uint64_t sense_other;
    uint64_t rereads;
    uint64_t sectors_recovered;
    uint64_t sectors_suspect;
    uint64_t slips;
    uint64_t subq_total;
    uint64_t subq_ok;
    uint16_t speed_requested_x;
    uint16_t speed_honoured_x;
    uint32_t subq_misposition;
    uint32_t buffer_peak_chunks;
    uint64_t buffer_stalls;
    ...;
} accudisc_read_stats;

int accudisc_read_cdda(accudisc_device *dev, const accudisc_read_req *req,
                       accudisc_sink_fn sink, void *user,
                       accudisc_read_stats *stats);

/* ---- MSF / Q ----------------------------------------------------------- */
int32_t accudisc_msf_to_lba(uint8_t m, uint8_t s, uint8_t f);
void accudisc_lba_to_msf(int32_t lba, uint8_t *m, uint8_t *s, uint8_t *f);
void accudisc_sub_extract_q(const uint8_t raw[96], uint8_t q[12]);

typedef struct accudisc_q {
    uint8_t adr;
    uint8_t control;
    uint8_t crc_ok;
    uint8_t tno;
    uint8_t index;
    uint8_t rel_m, rel_s, rel_f;
    uint8_t abs_m, abs_s, abs_f;
    char mcn[14];
    char isrc[13];
    ...;
} accudisc_q;

int accudisc_q_parse(const uint8_t q[12], accudisc_q *out);

/* ---- metadata ---------------------------------------------------------- */
int accudisc_scan_mcn(accudisc_device *dev, uint32_t lba, char mcn[14]);
int accudisc_scan_isrc(accudisc_device *dev, uint32_t lba, char isrc[13]);
"""
)

ffibuilder.set_source(
    "accudisc._accudisc",
    # The RUNPATH is stamped into the generated C as a comment, and that is
    # load-bearing rather than documentation. It protects the PIP path
    # specifically — `setup.py`'s cffi_modules, which runs setuptools'
    # build_ext and never reaches _remove_stale_extensions() in __main__.
    #
    # build_ext rebuilds on `newer_group(sources, target)` and cannot see
    # runtime_library_dirs, so with an unchanged cdef a build that changes ONLY
    # the RUNPATH reuses the cached object and relinks nothing. Stamping the
    # value into the source makes the .c content change, which makes cffi
    # rewrite it, which makes build_ext recompile — putting the thing that
    # decides correctness back into the cache key.
    #
    # Measured, and measured BOTH ways, because the first test could not tell
    # the difference: running this script directly self-cleans, so it relinks
    # regardless and shows nothing. On the pip path, two installs from one
    # source tree requesting /opt/aaa then /opt/bbb give
    #
    #     with this stamp:    /opt/aaa, then /opt/bbb   (correct)
    #     without it:         /opt/aaa, then /opt/aaa   (silently stale)
    #
    # The real-world instance: a `pip install` with a stale
    # bindings/python/build/ produced a wheel whose RUNPATH still named the
    # development build tree, while every log line said success.
    "/* accudisc runtime_library_dirs: %s */\n"
    "#include <accudisc/accudisc.h>" % (":".join(_RUNTIME_DIRS) or "(none)"),
    libraries=["accudisc"],
    include_dirs=_INCLUDE_DIRS,
    library_dirs=_LIBRARY_DIRS,
    runtime_library_dirs=_RUNTIME_DIRS,
    # One artefact for every CPython >= 3.2, instead of one per interpreter.
    #
    # This is what makes _remove_stale_extensions' rule ("exactly one extension
    # and it is always current") actually sufficient. Without it the rule is
    # still true and still not enough, because extensions are per-interpreter:
    # the last interpreter to build becomes the only one that can import, and
    # a consumer on a different minor gets ModuleNotFoundError. cdda2img hit
    # exactly that (§114.1) the moment the cleanup landed — their venv is 3.10,
    # ours is 3.14, and this package's declared floor is 3.10.
    #
    # cffi's API-mode output is limited-API already: it defines Py_LIMITED_API
    # in the generated source itself, and every Python symbol the extension
    # references is in the stable ABI (checked with `nm -D --undefined-only`).
    # This flag is what makes setuptools NAME it accordingly and stop tagging
    # it for one interpreter. Verified rather than assumed: an extension built
    # on 3.14 imports and passes the suite on 3.10.20.
    py_limited_api=True,
)


def _remove_stale_extensions(keep: Path | None = None) -> list[Path]:
    """Delete every ``_accudisc*.so`` in the package except *keep*.

    Found by cdda2img (§113) as a live hazard, not a tidiness one. A build here
    produces ONE interpreter-tagged extension — but an earlier ``pip install .``
    left an ``_accudisc.abi3.so`` behind, and the two then diverge silently:
    a rebuild refreshes the tagged file while the abi3 one stays as it was.
    Which file gets loaded depends on the interpreter, so on 3.14 you always
    get today's build and on 3.10 (this package's floor, and cdda2img's venv)
    you get whatever was last installed. Theirs was two days stale, against a
    library rebuilt three times that morning.

    Nothing catches that downstream. ``_check_version_skew`` compares versions,
    and a struct layout can change without one moving — which is exactly what
    had happened. So the fix is here: leave one extension, or none. A missing
    module is an ImportError, which is loud; a stale one is well-formed calls
    about the wrong bytes.

    Scope is deliberately narrow: only ``_accudisc*.so``, only in the package
    directory, all of them outputs of this script and git-ignored.
    """
    pkg = _HERE / "accudisc"
    removed = []
    for so in sorted(pkg.glob("_accudisc*.so")):
        if keep is not None and so.resolve() == keep.resolve():
            continue
        so.unlink()
        removed.append(so)
    return removed


def _main() -> None:
    import argparse

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--stage",
        metavar="DIR",
        help="compile into DIR instead of this source tree, and leave the "
        "source tree's extension alone. Used by `make install` to build an "
        "extension whose RUNPATH points at the INSTALLED libdir, without "
        "disturbing the development one that points at build/src — the two "
        "differ only in RUNPATH, so overwriting one with the other would "
        "silently break either the test suite or the install.",
    )
    args = ap.parse_args()

    if args.stage:
        stage = Path(args.stage).resolve()
        stage.mkdir(parents=True, exist_ok=True)
        built = Path(ffibuilder.compile(tmpdir=str(stage), verbose=True))
        # No _remove_stale_extensions here, deliberately: its scope is the
        # source package directory, and a staged build must not touch it.
        # The staging directory is created fresh by the build system instead.
        print(f"staged {built}")
        return

    # Before, not after: a failed compile must not leave the previous build in
    # place looking current. Better to end with no extension than a stale one.
    for gone in _remove_stale_extensions():
        print(f"removed stale extension {gone.name}")
    built = Path(ffibuilder.compile(verbose=True))
    for gone in _remove_stale_extensions(keep=built):
        print(f"removed stale extension {gone.name}")
    print(f"built {built.name}")


if __name__ == "__main__":
    _main()
