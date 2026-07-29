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

(3) is the working default today because the library is not installed yet
(TODO "install properly"). It also sets an RPATH at (3), so the extension finds
``libaccudisc.so.0`` without ``LD_LIBRARY_PATH``.
"""

from __future__ import annotations

import os
import shutil
import subprocess
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


def _locate() -> tuple[list[str], list[str], list[str]]:
    """Return (include_dirs, library_dirs, runtime_library_dirs)."""
    inc = os.environ.get("ACCUDISC_INCLUDE_DIR")
    lib = os.environ.get("ACCUDISC_LIB_DIR")
    if inc and lib:
        return [inc], [lib], [lib]

    pc_inc = [a[2:] for a in _pkg_config("--cflags-only-I") if a.startswith("-I")]
    pc_lib = [a[2:] for a in _pkg_config("--libs-only-L") if a.startswith("-L")]
    if pc_inc and pc_lib:
        return pc_inc, pc_lib, pc_lib

    # Uninstalled: build against this checkout.
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
    return [str(tree_inc)], [str(tree_lib)], [str(tree_lib)]


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
    ACCUDISC_ERR_UNSAFE_COMBINATION,
    ACCUDISC_ERR_ABI,
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
    ...;
} accudisc_features;

int accudisc_probe_features(accudisc_device *dev, accudisc_features *out);
int accudisc_probe_accurate_stream(accudisc_device *dev, uint32_t lba,
                                   uint8_t *accurate);

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
    uint8_t allow_unsafe;
    uint8_t *status_map;
    const volatile int *cancel;
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
    "#include <accudisc/accudisc.h>",
    libraries=["accudisc"],
    include_dirs=_INCLUDE_DIRS,
    library_dirs=_LIBRARY_DIRS,
    runtime_library_dirs=_RUNTIME_DIRS,
)


if __name__ == "__main__":
    ffibuilder.compile(verbose=True)
