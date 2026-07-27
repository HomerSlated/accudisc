"""accudisc — Python binding for libaccudisc.

Built against ``include/accudisc/accudisc.h`` only, never ``src/`` internals
(CLAUDE.md, "the public header is the contract"). The C library moves bits and
nothing else; this binding adds Python types and lifetime enforcement, and no
policy of its own.

What this binding deliberately does NOT do (API_PLAN §3, §7.3): shell out,
re-parse ``--progress-fd`` lines, or reproduce the CLI's exit codes. Those are
*process* conventions and live in the CLI by design; the mapping is
``docs/reference/cli-machine-interface.md``.

Three ways to get audio out, in the order you should reach for them:

* :meth:`Device.read_span` — bounded range, returns ``bytes``. The
  highest-value call: no sink, no temp file, no filesystem round-trip.
* :meth:`Device.read` — the raw sink, for spans too large to hold.
* :meth:`Device.read_to_file` — a sink that writes the streams to files.
  Provided for completeness; for a *whole disc* the CLI's ``--pcm`` path is
  still the better transport, because it writes the file inside the library's
  own address space with no per-chunk trip through Python.

Error convention, from ``cli-machine-interface.md``:

* ``ACCUDISC_ERR_NOTFOUND`` is **absence, not failure** — the calls where a
  thing can legitimately be missing (MCN, ISRC, CD-Text, read offset) return
  ``None`` rather than raising.
* ``accudisc_write``'s positive return means the burn **completed** with a
  caveat. Nothing here turns a positive return into an exception.
* ``ACCUDISC_ERR_ABI`` raises :class:`AbiMismatch`, deliberately distinct from
  :class:`InvalidArgument`: it means *rebuild this binding against the library
  you have loaded*, not *fix your arguments*.
"""

from __future__ import annotations

import enum
import os
from dataclasses import dataclass, field
from typing import Callable, Sequence

from ._accudisc import ffi, lib

__all__ = [
    "AccuDiscError", "InvalidArgument", "OutOfMemory", "OpenFailed", "IOFailed",
    "SenseError", "ShortResponse", "Unsupported", "Cancelled", "CrcError",
    "NotFound", "UnsafeCombination", "AbiMismatch", "RetainedBufferError",
    "C2", "Sub", "MapState", "Anomaly", "TocSource", "TocDegrade", "C2Verdict",
    "Sense", "DriveId", "Track", "Session", "Toc", "TocInfo", "Features",
    "Chunk", "ReadStats", "ReadResult", "Cancel", "Device", "Q",
    "version", "library_version", "map_state", "map_severity", "anomaly_token",
    "msf_to_lba", "lba_to_msf", "parse_q", "extract_q",
    "MAX_SPAN_BYTES", "UNTRUSTED_GEOMETRY",
]

# ---------------------------------------------------------------------------
# version
# ---------------------------------------------------------------------------

#: Version of the header this extension was COMPILED against.
version = (
    lib.ACCUDISC_VERSION_MAJOR,
    lib.ACCUDISC_VERSION_MINOR,
    lib.ACCUDISC_VERSION_PATCH,
)


def library_version() -> tuple[int, int, int]:
    """Version of the ``libaccudisc.so`` actually LOADED.

    The header documents comparing this against :data:`version` to detect
    header/library skew. The two are single-sourced in CMake (the .so version
    is parsed out of the header), so a mismatch means a stale ``.so`` is being
    picked up — a deployment failure, not a theoretical one.
    """
    maj = ffi.new("int*")
    minor = ffi.new("int*")
    patch = ffi.new("int*")
    lib.accudisc_version(maj, minor, patch)
    return (maj[0], minor[0], patch[0])


def version_string() -> str:
    """The loaded library's version as it prints it, e.g. ``"0.2.0"``."""
    return ffi.string(lib.accudisc_version_string()).decode()


def _check_version_skew() -> None:
    compiled, loaded = version, library_version()
    if compiled[:2] != loaded[:2]:
        raise AbiMismatch(
            lib.ACCUDISC_ERR_ABI,
            f"binding compiled against accudisc {'.'.join(map(str, compiled))} "
            f"but loaded libaccudisc {'.'.join(map(str, loaded))} — rebuild "
            f"the binding (python build_accudisc.py)",
        )


# ---------------------------------------------------------------------------
# errors
# ---------------------------------------------------------------------------


class AccuDiscError(Exception):
    """Base for every ``accudisc_err``. Carries the numeric ``code``."""

    code: int = 0

    def __init__(self, code: int, message: str | None = None,
                 sense: "Sense | None" = None, io_detail: str = ""):
        self.code = int(code)
        self.name = ffi.string(lib.accudisc_strerror(self.code)).decode()
        self.sense = sense
        self.io_detail = io_detail
        if message is None:
            message = self.name
            if sense is not None and sense.valid:
                message += f" ({sense})"
            elif io_detail:
                message += f" ({io_detail})"
        super().__init__(message)


class InvalidArgument(AccuDiscError):
    """``ACCUDISC_ERR_INVAL`` — fix your arguments."""


class OutOfMemory(AccuDiscError):
    """``ACCUDISC_ERR_NOMEM``."""


class OpenFailed(AccuDiscError):
    """``ACCUDISC_ERR_OPEN`` — the device node could not be opened."""


class IOFailed(AccuDiscError):
    """``ACCUDISC_ERR_IO`` — transport/host/driver failure, no sense data.

    ``io_detail`` carries ``accudisc_last_io()``, without which a transport
    failure cannot be attributed after the fact.
    """


class SenseError(AccuDiscError):
    """``ACCUDISC_ERR_SENSE`` — the drive returned CHECK CONDITION."""


class ShortResponse(AccuDiscError):
    """``ACCUDISC_ERR_SHORT``."""


class Unsupported(AccuDiscError):
    """``ACCUDISC_ERR_UNSUPPORTED`` — not supported by this drive or build."""


class Cancelled(AccuDiscError):
    """``ACCUDISC_ERR_CANCELLED`` — stopped by the cancel flag or the sink."""


class CrcError(AccuDiscError):
    """``ACCUDISC_ERR_CRC`` — a Q frame or CD-Text pack failed its checksum."""


class NotFound(AccuDiscError):
    """``ACCUDISC_ERR_NOTFOUND`` — the data is legitimately ABSENT.

    Raised only where the caller asked for something whose absence is an error
    in context. The metadata accessors return ``None`` instead; absence is not
    failure (``cli-machine-interface.md``).
    """


class UnsafeCombination(AccuDiscError):
    """``ACCUDISC_ERR_UNSAFE_COMBINATION`` — a measured-corrupt request.

    Today the only case is capturing subchannel with the vendor read-speed
    uncap on, which silently corrupts Q. Override with ``allow_unsafe=True``
    for diagnostics only.
    """


class AbiMismatch(AccuDiscError):
    """``ACCUDISC_ERR_ABI`` — **rebuild the binding**, do not fix arguments.

    A distinct type on purpose: this is the failure a partial upgrade produces
    (new ``.so``, stale extension module), and a caller that wants to fall back
    to another transport needs to catch exactly this and nothing else.
    """


class RetainedBufferError(AccuDiscError):
    """A zero-copy chunk view outlived the sink call that produced it.

    ``chunk.data`` is valid only for the duration of the sink call. A retained
    view reads freed memory as plausible PCM — well-formed, wrong referent, no
    exception, no C2 flag.

    Raised when a sub-view was still exported as the sink returned, so the
    handed-out view could not be released. Note this does NOT catch every
    escape route — see :class:`_SinkTrampoline` for exactly what is and is not
    enforced.
    """

    def __init__(self, message: str):
        super().__init__(lib.ACCUDISC_ERR_INVAL, message)


_ERRORS: dict[int, type[AccuDiscError]] = {
    lib.ACCUDISC_ERR_INVAL: InvalidArgument,
    lib.ACCUDISC_ERR_NOMEM: OutOfMemory,
    lib.ACCUDISC_ERR_OPEN: OpenFailed,
    lib.ACCUDISC_ERR_IO: IOFailed,
    lib.ACCUDISC_ERR_SENSE: SenseError,
    lib.ACCUDISC_ERR_SHORT: ShortResponse,
    lib.ACCUDISC_ERR_UNSUPPORTED: Unsupported,
    lib.ACCUDISC_ERR_CANCELLED: Cancelled,
    lib.ACCUDISC_ERR_CRC: CrcError,
    lib.ACCUDISC_ERR_NOTFOUND: NotFound,
    lib.ACCUDISC_ERR_UNSAFE_COMBINATION: UnsafeCombination,
    lib.ACCUDISC_ERR_ABI: AbiMismatch,
}


def _raise(rc: int, dev: "Device | None" = None) -> None:
    """Turn a negative ``accudisc_err`` into the matching exception."""
    sense = dev._sense() if dev is not None and rc == lib.ACCUDISC_ERR_SENSE else None
    detail = dev._last_io() if dev is not None and rc == lib.ACCUDISC_ERR_IO else ""
    raise _ERRORS.get(int(rc), AccuDiscError)(rc, sense=sense, io_detail=detail)


def _check(rc: int, dev: "Device | None" = None) -> int:
    """Raise on a negative return; pass non-negative straight through.

    Non-negative, not ``== ACCUDISC_OK``: ``accudisc_write`` returns a POSITIVE
    value meaning "the burn completed, with a caveat", and turning that into an
    exception would report a completed burn as a failure.
    """
    if rc < 0:
        _raise(rc, dev)
    return rc


# ---------------------------------------------------------------------------
# enums
# ---------------------------------------------------------------------------


class C2(enum.IntEnum):
    """``req.c2`` — per-sector C2 error pointers to request alongside audio."""

    NONE = lib.ACCUDISC_C2_NONE
    PTRS = lib.ACCUDISC_C2_PTRS
    PTRS_BEB = lib.ACCUDISC_C2_PTRS_BEB


class Sub(enum.IntEnum):
    """``req.sub`` — subchannel to request alongside audio."""

    NONE = lib.ACCUDISC_SUB_NONE
    RAW = lib.ACCUDISC_SUB_RAW
    Q = lib.ACCUDISC_SUB_Q


class MapState(enum.IntEnum):
    """Low nibble of a status-map byte.

    Every state is a **relative** claim — "stable/clean across the reads of
    this run" — never verification against the pressing's canonical bytes. A
    drive that misreads deterministically passes every relative check; the
    absolute gates (AccurateRip, CTDB) are the calling application's job and
    always outrank anything recorded here.
    """

    PENDING = lib.ACCUDISC_MAP_PENDING
    OK = lib.ACCUDISC_MAP_OK
    C2 = lib.ACCUDISC_MAP_C2
    HARD = lib.ACCUDISC_MAP_HARD
    RECOVERED = lib.ACCUDISC_MAP_RECOVERED
    SUSPECT = lib.ACCUDISC_MAP_SUSPECT


def map_state(b: int) -> MapState:
    """State nibble of a status-map byte."""
    return MapState(b & 0x0F)


def map_severity(b: int) -> int:
    """Severity nibble of a status-map byte.

    **Not comparable across states.** The unit differs per state: for ``C2`` it
    is ~log2 of the fired C2 bit count, for ``SUSPECT`` ~log2 of the
    disagreeing byte count, and for ``RECOVERED`` it is a raw reread count.
    Ranking sectors by this number across mixed states compares three different
    quantities.
    """
    return (b >> 4) & 0x0F


class Anomaly(enum.IntFlag):
    """``toc.anomalies`` — structural defects found parsing the lead-in.

    Copy-protection schemes work by deliberately malforming the TOC, so these
    are split by CONSEQUENCE: :data:`UNTRUSTED_GEOMETRY` means the track map
    cannot be relied on to say which sectors are audio.
    """

    LBA_ORDER = lib.ACCUDISC_TOC_ANOM_LBA_ORDER
    OVERLAP = lib.ACCUDISC_TOC_ANOM_OVERLAP
    LEADOUT_BEFORE = lib.ACCUDISC_TOC_ANOM_LEADOUT_BEFORE
    PAST_LEADOUT = lib.ACCUDISC_TOC_ANOM_PAST_LEADOUT
    EMPTY_TRACK = lib.ACCUDISC_TOC_ANOM_EMPTY_TRACK
    NEGATIVE_LBA = lib.ACCUDISC_TOC_ANOM_NEGATIVE_LBA
    BAD_TRACK_NUM = lib.ACCUDISC_TOC_ANOM_BAD_TRACK_NUM
    RANGE_MISMATCH = lib.ACCUDISC_TOC_ANOM_RANGE_MISMATCH
    BAD_SESSION = lib.ACCUDISC_TOC_ANOM_BAD_SESSION


#: The anomalies that make the track map untrustworthy (a mask, not a bit).
UNTRUSTED_GEOMETRY = Anomaly(lib.ACCUDISC_TOC_ANOM_UNTRUSTED_GEOMETRY)


def anomaly_token(bit: Anomaly | int) -> str:
    """Stable lowercase slug for ONE anomaly bit, e.g. ``"lba_order"``.

    Returns ``"unknown"`` for a composite value — pass single bits.
    """
    return ffi.string(lib.accudisc_toc_anomaly_str(int(bit))).decode()


class TocSource(enum.IntEnum):
    """Which physical operation answered — not two views of one thing.

    ``FULLTOC`` (READ TOC format 2) replays the raw Q-channel of the lead-in
    and is the only source of session structure; ``TOC`` (format 0) returns the
    drive's already-decoded track list.
    """

    FULLTOC = 0
    TOC = 1

    @property
    def token(self) -> str:
        """The stable lowercase machine token: ``"fulltoc"`` / ``"toc"``."""
        return ffi.string(lib.accudisc_toc_source_str(int(self))).decode()


class TocDegrade(enum.IntEnum):
    """Why format 2 was not used. A DISC-HEALTH signal, not plumbing.

    A lead-in that has become unreadable while the program area is still
    perfect predicts what fails next, so it is surfaced rather than hidden.
    """

    NONE = 0
    LEADIN_UNREADABLE = 1
    LEADIN_ABSENT = 2
    LEADIN_MALFORMED = 3

    @property
    def token(self) -> str:
        return ffi.string(lib.accudisc_toc_degrade_str(int(self))).decode()


class C2Verdict(enum.IntEnum):
    """Conservative: only "claimed AND functional" earns ``SUPPORTED``."""

    UNSUPPORTED = lib.ACCUDISC_C2_UNSUPPORTED
    SUPPORTED = lib.ACCUDISC_C2_SUPPORTED
    UNVERIFIED = lib.ACCUDISC_C2_UNVERIFIED


# ---------------------------------------------------------------------------
# value types
# ---------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class Sense:
    """Decoded SCSI sense from the most recent failed command."""

    valid: bool
    key: int
    asc: int
    ascq: int

    def __str__(self) -> str:
        if not self.valid:
            return "no sense"
        return f"sense key {self.key:#x} asc {self.asc:#04x} ascq {self.ascq:#04x}"


@dataclass(frozen=True, slots=True)
class DriveId:
    """INQUIRY strings, space-trimmed."""

    vendor: str
    product: str
    revision: str

    def __str__(self) -> str:
        return f"{self.vendor} {self.product} {self.revision}".strip()


@dataclass(frozen=True, slots=True)
class Track:
    """One TOC track entry."""

    number: int
    adr_ctrl: int
    session: int
    lba: int
    sectors: int
    pregap: int

    @property
    def is_audio(self) -> bool:
        """``ACCUDISC_TRACK_IS_AUDIO`` — CTRL bit 2 clear."""
        return (self.adr_ctrl & 0x04) == 0

    @property
    def is_data(self) -> bool:
        return not self.is_audio

    @property
    def pre_emphasis(self) -> bool:
        """CTRL bit 0."""
        return bool(self.adr_ctrl & 0x01)

    @property
    def full_extent(self) -> tuple[int, int]:
        """``(start, count)`` INCLUDING the pregap that belongs to this track.

        ECMA-130 §20: a Pause is "a part of an Information Track", so the
        sectors before INDEX 01 belong to the track that follows them. A rip
        that starts at ``lba`` silently drops them, shifting every LBA against
        the audio stream and producing a wrong disc ID.
        """
        return (self.lba - self.pregap, self.sectors + self.pregap)


@dataclass(frozen=True, slots=True)
class Session:
    """One session's identity and extent."""

    number: int
    first_track: int
    last_track: int
    audio_tracks: int
    data_tracks: int
    leadout_lba: int


@dataclass(frozen=True, slots=True)
class Toc:
    """A parsed TOC.

    Track extents are bounded by the OWNING SESSION's lead-out, never by the
    next track start on the disc: between two sessions sit a lead-out, a
    lead-in and a pregap — on a typical Enhanced CD ~11,400 sectors that hold
    no track payload and cannot be read as CD-DA.
    """

    first_track: int
    last_track: int
    tracks: tuple[Track, ...]
    sessions: tuple[Session, ...]
    leadout_lba: int
    anomalies: Anomaly
    sessions_total: int

    @property
    def trusted(self) -> bool:
        """False when the track map cannot be relied on to say what is audio.

        This is a **gate**, not a display field: build geometry only when it is
        true. A safety gate that loses its input does not degrade to "less
        informative", it degrades to a wrong disc ID reported silently.
        """
        return not (self.anomalies & UNTRUSTED_GEOMETRY)

    @property
    def mapped_session_count(self) -> int:
        """Sessions whose track ownership and end we KNOW.

        ``0`` means the source could not report session structure (the format-0
        degrade path) — **not** that the disc has one session. Compare with
        :attr:`sessions_total`.
        """
        return len(self.sessions)

    @property
    def audio_tracks(self) -> tuple[Track, ...]:
        return tuple(t for t in self.tracks if t.is_audio)

    @property
    def data_tracks(self) -> tuple[Track, ...]:
        return tuple(t for t in self.tracks if t.is_data)

    def track(self, number: int) -> Track:
        for t in self.tracks:
            if t.number == number:
                return t
        raise KeyError(f"no track {number} in TOC")


@dataclass(frozen=True, slots=True)
class TocInfo:
    """How the TOC was obtained, and how many sessions the DISC has.

    Note the three different session counts, which must not be conflated:

    * :attr:`Toc.mapped_session_count` — sessions we can MAP (0 = unknown)
    * :attr:`Toc.sessions_total` — how many the disc HAS, from whichever source
      could say (0 = nobody could say)
    * :attr:`session_count` here — from READ DISC INFORMATION, a different
      opcode answered from the drive's own disc model, so it survives an
      unreadable lead-in (``None`` = unobtainable)

    ``sessions_total > mapped_session_count`` is the honest description of a
    degraded read of a multi-session disc: the seams exist but we do not know
    where they fall, which is strictly more dangerous than knowing nothing.
    """

    source: TocSource
    degrade: TocDegrade
    degrade_err: int
    first_session: int
    last_session: int
    session_count: int | None
    disc_type: int

    @property
    def degraded(self) -> bool:
        return self.degrade is not TocDegrade.NONE


@dataclass(frozen=True, slots=True)
class Features:
    """What the drive CLAIMS (GET CONFIGURATION) versus what it DOES."""

    feature_present: bool
    current: bool
    dap: bool
    c2_claimed: bool
    cdtext_claimed: bool
    ok_c2: bool
    ok_sub_raw: bool
    ok_sub_q: bool
    ok_c2_sub_raw: bool
    ok_c2_sub_q: bool
    c2_verdict: C2Verdict

    @property
    def combos(self) -> dict[str, bool]:
        """The functional smoke-read results, keyed as the CLI names them."""
        return {
            "c2": self.ok_c2,
            "sub_raw": self.ok_sub_raw,
            "sub_q": self.ok_sub_q,
            "c2_sub_raw": self.ok_c2_sub_raw,
            "c2_sub_q": self.ok_c2_sub_q,
        }


@dataclass(frozen=True, slots=True)
class ReadStats:
    """Counters for one :meth:`Device.read`."""

    sectors_read: int
    sectors_flagged: int
    c2_bits: int
    hard_errors: int
    max_bits_sector: int
    first_flagged_lba: int
    last_flagged_lba: int
    sense_medium: int
    sense_hardware: int
    sense_other: int
    rereads: int
    sectors_recovered: int
    sectors_suspect: int
    slips: int
    subq_total: int
    subq_ok: int

    @property
    def subq_bad(self) -> int:
        """Pregap/index/MSF metadata lost on this pass.

        The subchannel has no CIRC protection — a per-frame CRC-16 is its only
        integrity check, and it fails independently of the audio C2 counters.
        """
        return self.subq_total - self.subq_ok


@dataclass(slots=True)
class Chunk:
    """One delivered chunk, valid ONLY during the sink call.

    ``data`` holds ``nsec`` sectors of ``sector_len`` bytes each, laid out
    AUDIO (``audio_len``) + C2 (``c2_len``) + SUB (``sub_len``).
    Hard-unreadable sectors arrive zero-filled with an all-ones C2 bitmap, so
    the streams never desync.

    In the default (copying) mode ``data`` is ``bytes`` and yours to keep. In
    zero-copy mode it is a ``memoryview`` over library memory that is released
    the moment your sink returns.
    """

    lba: int
    nsec: int
    data: bytes | memoryview
    sector_len: int
    audio_len: int
    c2_len: int
    sub_len: int

    def sector(self, i: int) -> memoryview | bytes:
        """Sector ``i`` of this chunk, whole (audio + C2 + sub)."""
        off = i * self.sector_len
        return self.data[off:off + self.sector_len]

    def audio(self, i: int) -> memoryview | bytes:
        """Just the PCM of sector ``i``."""
        off = i * self.sector_len
        return self.data[off:off + self.audio_len]


# ---------------------------------------------------------------------------
# pure helpers (no device needed)
# ---------------------------------------------------------------------------


def msf_to_lba(m: int, s: int, f: int) -> int:
    """MSF as it appears on disc. LBA 0 == 00:02:00 (the 150-sector pregap)."""
    return int(lib.accudisc_msf_to_lba(m, s, f))


def lba_to_msf(lba: int) -> tuple[int, int, int]:
    """Inverse of :func:`msf_to_lba`.

    An LBA below -150 (deep lead-in) is before 00:00:00, which MSF cannot
    represent, so it clamps to 00:00:00 rather than wrapping.
    """
    m = ffi.new("uint8_t*")
    s = ffi.new("uint8_t*")
    f = ffi.new("uint8_t*")
    lib.accudisc_lba_to_msf(lba, m, s, f)
    return (m[0], s[0], f[0])


def extract_q(raw96: bytes) -> bytes:
    """Deinterleave the 12-byte Q frame out of 96 bytes of raw P-W subcode."""
    if len(raw96) != 96:
        raise ValueError(f"raw subcode must be 96 bytes, got {len(raw96)}")
    out = ffi.new("uint8_t[12]")
    lib.accudisc_sub_extract_q(raw96, out)
    return bytes(ffi.buffer(out, 12))


@dataclass(frozen=True, slots=True)
class Q:
    """A decoded Q-subchannel frame.

    Position and MCN/ISRC fields are populated **only** when ``crc_ok``; a bad
    frame yields out-of-range BCD, so they are left zero rather than decoded.
    """

    adr: int
    control: int
    crc_ok: bool
    tno: int
    index: int
    rel: tuple[int, int, int]
    abs: tuple[int, int, int]
    mcn: str
    isrc: str

    @property
    def is_pregap(self) -> bool:
        """INDEX 00 — the pause belonging to the track that follows."""
        return self.index == 0


def parse_q(q12: bytes) -> Q:
    """Decode a 12-byte Q frame. Raises :class:`CrcError` if the CRC fails."""
    if len(q12) != 12:
        raise ValueError(f"Q frame must be 12 bytes, got {len(q12)}")
    out = ffi.new("accudisc_q*")
    _check(lib.accudisc_q_parse(q12, out))
    return Q(
        adr=out.adr,
        control=out.control,
        crc_ok=bool(out.crc_ok),
        tno=out.tno,
        index=out.index,
        rel=(out.rel_m, out.rel_s, out.rel_f),
        abs=(out.abs_m, out.abs_s, out.abs_f),
        mcn=ffi.string(out.mcn).decode("ascii", "replace"),
        isrc=ffi.string(out.isrc).decode("ascii", "replace"),
    )


# ---------------------------------------------------------------------------
# reading
# ---------------------------------------------------------------------------

#: Default ceiling for :meth:`Device.read_span`, which materialises the whole
#: span in memory. Above this you are asked to use the sink instead — an
#: accidental whole-disc ``read_span`` would otherwise be a ~1 GB allocation
#: that looks like a typo.
MAX_SPAN_BYTES = 128 * 1024 * 1024

SinkFn = Callable[["Chunk"], None]


class _SinkTrampoline:
    """Bridges a Python sink to ``accudisc_sink_fn``.

    Two hazards handled here, both measured rather than assumed:

    1. **A raised exception must not read as "continue".** cffi's default
       callback prints the traceback and returns 0 — and 0 is exactly what the
       engine reads as *keep going*. So the callback is built with ``error=1``
       (nonzero cancels) and ``onerror=`` to stash the exception, which is
       re-raised after ``accudisc_read_cdda`` returns.

    2. **A retained zero-copy view outlives its buffer.** The view is released
       when the sink returns, so a stored reference raises ``ValueError`` on
       its next use rather than reading freed memory as plausible PCM.

       The limit of that, stated plainly because a guard believed to be
       stronger than it is is worse than none: releasing the view we handed
       out does **not** reach a *slice* the sink took of it. Measured — after
       ``parent.release()`` a slice of it is still readable, because a
       memoryview slice re-exports from the underlying buffer rather than from
       the parent view. A refcount-based detector for that case was built and
       withdrawn: it read differently depending on the calling frame and
       flagged correct code, and a false alarm in a guard is how the guard
       stops being believed.

       So the enforcement is: default to copying, make zero-copy opt-in, catch
       the common retention (storing ``chunk.data``), and be explicit that
       slicing escapes it.
    """

    def __init__(self, sink: SinkFn, copy: bool):
        self._sink = sink
        self._copy = copy
        self.error: BaseException | None = None
        self.cffi_callback = ffi.callback(
            "int(void*, accudisc_chunk*)", self._call, error=1,
            onerror=self._onerror,
        )

    def _onerror(self, exc, value, tb) -> None:
        if self.error is None:
            self.error = value

    def _call(self, _user, c) -> int:
        nbytes = c.nsec * c.sector_len
        if self._copy:
            data = bytes(ffi.buffer(c.data, nbytes))
            self._sink(Chunk(c.lba, c.nsec, data, c.sector_len,
                             c.audio_len, c.c2_len, c.sub_len))
            return 0

        view = memoryview(ffi.buffer(c.data, nbytes))
        try:
            self._sink(Chunk(c.lba, c.nsec, view, c.sector_len,
                             c.audio_len, c.c2_len, c.sub_len))
        finally:
            try:
                view.release()
            except BufferError as exc:
                # A sub-view is still exported. We cannot invalidate it, so
                # say so rather than leaving it to become stale PCM.
                raise RetainedBufferError(
                    f"a zero-copy view of the chunk at LBA {c.lba} is still "
                    f"exported when the sink returned ({exc}). chunk.data is "
                    f"valid only during the call — copy what you need "
                    f"(bytes(chunk.data)) or use copy=True."
                ) from None
        return 0


class Cancel:
    """A cancellation flag polled by the engine at each chunk boundary.

    ``accudisc_read_cdda`` blocks, so set this from the sink or another thread.
    """

    def __init__(self) -> None:
        self._flag = ffi.new("int*")

    def cancel(self) -> None:
        self._flag[0] = 1

    @property
    def cancelled(self) -> bool:
        return bool(self._flag[0])


@dataclass(slots=True)
class ReadResult:
    """What one :meth:`Device.read` produced."""

    lba: int
    count: int
    stats: ReadStats
    _map: object = None
    _keepalive: list = field(default_factory=list)

    @property
    def status_map(self) -> memoryview | None:
        """Per-sector status bytes, or ``None`` if not requested.

        Byte *i* is LBA ``self.lba + i``. Decode with :func:`map_state` and
        :func:`map_severity`. Byte-identical in layout to the CLI's
        ``--map-file``: the CLI mmaps that file and hands the mapping to the
        library as this same buffer (``cli/main.c:1396``), so one decoder
        serves both.
        """
        if self._map is None:
            return None
        return memoryview(ffi.buffer(self._map, self.count))

    def state_counts(self) -> dict[MapState, int]:
        """Census of the status map by state."""
        mv = self.status_map
        if mv is None:
            return {}
        counts = dict.fromkeys(MapState, 0)
        for b in mv:
            counts[MapState(b & 0x0F)] += 1
        return counts


# ---------------------------------------------------------------------------
# device
# ---------------------------------------------------------------------------


class Device:
    """One optical drive.

    Handles are **not thread-safe**: one per thread, or serialise externally.
    Reading a status map while another thread drives the device IS safe, and is
    the intended progress-tracking pattern.

    .. warning::
       This machine shares one drive between agents. Take
       ``flock /var/tmp/sr0.lock`` around any use of ``/dev/sr0`` — measured
       contention collapses Q from 99% to 13% while the audio stays clean, so
       it presents as a bad disc rather than as a lock failure.
    """

    def __init__(self, path: str = "/dev/sr0", *, rdwr: bool = False):
        """Open the drive at ``path``.

        ``rdwr=True`` is required for vendor opcodes, MODE SELECT and writing:
        the kernel's unprivileged SG_IO command filter blocks those on
        read-only fds. Plain reading should leave it off (least privilege).
        """
        self._dev = None
        _check_version_skew()
        err = ffi.new("int*")
        flags = lib.ACCUDISC_OPEN_RDWR if rdwr else 0
        dev = lib.accudisc_open(path.encode(), flags, err)
        if dev == ffi.NULL:
            _raise(err[0])
        self._dev = dev
        self._path = path
        self._log_cb = None

    # -- lifecycle ---------------------------------------------------------

    def close(self) -> None:
        if getattr(self, "_dev", None) is not None:
            lib.accudisc_close(self._dev)
            self._dev = None

    def __enter__(self) -> "Device":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def __repr__(self) -> str:
        state = "closed" if getattr(self, "_dev", None) is None else "open"
        return f"<accudisc.Device {getattr(self, '_path', '?')!r} {state}>"

    @property
    def _handle(self):
        if getattr(self, "_dev", None) is None:
            raise ValueError("device is closed")
        return self._dev

    # -- diagnostics -------------------------------------------------------

    def _sense(self) -> Sense:
        out = ffi.new("accudisc_sense*")
        lib.accudisc_last_sense(self._handle, out)
        return Sense(bool(out.valid), out.key, out.asc, out.ascq)

    def _last_io(self) -> str:
        return ffi.string(lib.accudisc_last_io(self._handle)).decode()

    def set_log(self, fn: Callable[[str], None] | None) -> None:
        """Route library/driver diagnostics to ``fn`` (default: discarded)."""
        if fn is None:
            lib.accudisc_set_log(self._handle, ffi.NULL, ffi.NULL)
            self._log_cb = None
            return

        def trampoline(_user, msg):
            fn(ffi.string(msg).decode("utf-8", "replace"))

        # Held on the instance: if this were garbage collected the C library
        # would be left holding a dangling function pointer.
        self._log_cb = ffi.callback("void(void*, const char*)", trampoline)
        lib.accudisc_set_log(self._handle, self._log_cb, ffi.NULL)

    # -- identity ----------------------------------------------------------

    def identify(self) -> DriveId:
        out = ffi.new("accudisc_drive_id*")
        _check(lib.accudisc_drive_identify(self._handle, out), self)
        return DriveId(
            ffi.string(out.vendor).decode("ascii", "replace"),
            ffi.string(out.product).decode("ascii", "replace"),
            ffi.string(out.revision).decode("ascii", "replace"),
        )

    @property
    def read_offset(self) -> int | None:
        """Manufacturing read offset in samples, or ``None`` if unknown.

        Absence is not failure: an unlisted drive returns ``None``, and the
        caller decides whether to proceed with an uncorrected offset.
        """
        out = ffi.new("int32_t*")
        rc = lib.accudisc_read_offset(self._handle, out)
        if rc == lib.ACCUDISC_ERR_NOTFOUND:
            return None
        _check(rc, self)
        return out[0]

    @property
    def access_method(self) -> str:
        """``"generic MMC"`` or ``"driver <name> (<description>)"``."""
        return ffi.string(lib.accudisc_access_method(self._handle)).decode()

    def attach_driver(self, name: str | None = None,
                      directory: str | None = None) -> bool:
        """Grant permission to use a vendor driver. Returns False if none fits.

        Calling this IS the application's permission grant — without it a
        device never issues a vendor opcode. Order: identify → locate → load →
        selftest → use, and any failure falls back to generic MMC/SG. A missing
        driver file is never fatal, so this returns ``False`` rather than
        raising.

        Needs ``rdwr=True``: vendor opcodes require the kernel's full SG_IO
        command set or the selftest will fail.
        """
        rc = lib.accudisc_driver_attach(
            self._handle,
            name.encode() if name else ffi.NULL,
            directory.encode() if directory else ffi.NULL,
        )
        if rc in (lib.ACCUDISC_ERR_NOTFOUND, lib.ACCUDISC_ERR_UNSUPPORTED):
            return False
        _check(rc, self)
        return True

    def detach_driver(self) -> None:
        lib.accudisc_driver_detach(self._handle)

    # -- speed / mechanics -------------------------------------------------

    def set_speed(self, speed_x: int) -> None:
        """Best-effort read-speed ceiling, in Nx (176 kB/s units).

        Prefers SET STREAMING (0xB6, a ceiling the drive enforces; needs
        CAP_SYS_RAWIO), falling back to the unprivileged CDROM_SELECT_SPEED.
        """
        _check(lib.accudisc_set_speed(self._handle, speed_x), self)

    def get_speed(self) -> tuple[int, int]:
        """``(max_kbps, current_kbps)`` from mode page 2A. **kB/s, not Nx.**

        Divide by 176 for Nx. Note this is a *requested-speed ceiling*, not an
        achievable rate: measured whole-disc, a drive set to 48x and to 40x
        delivered 23.74x and 24.19x — 1.9% apart. Page 2A is not lying; it
        reports what you may ask for, and the CD-DA governor that decides what
        you get is enforced in the drive and not exposed over MMC.
        """
        max_kbps = ffi.new("unsigned*")
        cur_kbps = ffi.new("unsigned*")
        _check(lib.accudisc_get_speed(self._handle, max_kbps, cur_kbps), self)
        return (max_kbps[0], cur_kbps[0])

    def eject(self) -> None:
        _check(lib.accudisc_eject(self._handle), self)

    def load(self) -> None:
        _check(lib.accudisc_load(self._handle), self)

    def park_spindle(self) -> None:
        """Stop the spindle. Best-effort; a drive that refuses is not an error."""
        rc = lib.accudisc_spindle_stop(self._handle)
        if rc == lib.ACCUDISC_ERR_UNSUPPORTED:
            return
        _check(rc, self)

    # -- probes ------------------------------------------------------------

    def probe_features(self) -> Features:
        """What the drive claims vs what it does. Needs a disc loaded."""
        out = ffi.new("accudisc_features*")
        _check(lib.accudisc_probe_features(self._handle, out), self)
        return Features(
            feature_present=bool(out.feature_present),
            current=bool(out.current),
            dap=bool(out.dap),
            c2_claimed=bool(out.c2_claimed),
            cdtext_claimed=bool(out.cdtext_claimed),
            ok_c2=bool(out.ok_c2),
            ok_sub_raw=bool(out.ok_sub_raw),
            ok_sub_q=bool(out.ok_sub_q),
            ok_c2_sub_raw=bool(out.ok_c2_sub_raw),
            ok_c2_sub_q=bool(out.ok_c2_sub_q),
            c2_verdict=C2Verdict(out.c2_verdict),
        )

    def probe_accurate_stream(self, lba: int = 5000) -> bool:
        """Does this drive read audio positionally deterministically?

        Probe a CLEAN area — damage reads as jitter. ``False`` means the drive
        can slip, and boundary overlap checking is then the only defence
        against an error class C2 is structurally blind to.
        """
        out = ffi.new("uint8_t*")
        _check(lib.accudisc_probe_accurate_stream(self._handle, lba, out), self)
        return bool(out[0])

    # -- TOC ---------------------------------------------------------------

    def read_toc(self) -> Toc:
        """READ TOC format 0, parsed. Requires a disc."""
        out = ffi.new("accudisc_toc*")
        _check(lib.accudisc_read_toc(self._handle, out), self)
        return _toc_from_c(out)

    def read_toc_src(self) -> tuple[Toc, TocInfo]:
        """TOC plus the acquisition path that produced it.

        Prefers format 2 and degrades to format 0, reporting which answered and
        why. Succeeds if EITHER path produced a usable TOC; only a failure of
        both raises.
        """
        out = ffi.new("accudisc_toc*")
        info = ffi.new("accudisc_toc_info*")
        _check(lib.accudisc_read_toc_src(self._handle, out, info), self)
        return (
            _toc_from_c(out),
            TocInfo(
                source=TocSource(info.source),
                degrade=TocDegrade(info.degrade),
                degrade_err=info.degrade_err,
                first_session=info.first_session,
                last_session=info.last_session,
                session_count=info.session_count or None,
                disc_type=info.disc_type,
            ),
        )

    def read_full_toc_raw(self) -> bytes:
        """Raw READ TOC format 2 response, undecoded (feed your own parser)."""
        return self._library_blob(lib.accudisc_read_full_toc)

    def read_cdtext_raw(self) -> bytes | None:
        """Raw CD-Text packs from the lead-in (READ TOC format 5), undecoded.

        ``None`` when the disc carries no CD-Text — absence, not failure. A
        drive that *rejects* format 5 outright still raises
        :class:`SenseError`, deliberately not conflated with "absent".
        """
        try:
            return self._library_blob(lib.accudisc_read_cdtext)
        except NotFound:
            return None

    def _library_blob(self, fn) -> bytes:
        out = ffi.new("uint8_t**")
        n = ffi.new("uint32_t*")
        _check(fn(self._handle, out, n), self)
        try:
            return bytes(ffi.buffer(out[0], n[0]))
        finally:
            # Route through the library's own free, never Python's.
            lib.accudisc_free(out[0])

    # -- metadata ----------------------------------------------------------

    def scan_mcn(self, lba: int) -> str | None:
        """Disc MCN, or ``None`` if the disc does not carry one."""
        out = ffi.new("char[14]")
        rc = lib.accudisc_scan_mcn(self._handle, lba, out)
        if rc == lib.ACCUDISC_ERR_NOTFOUND:
            return None
        _check(rc, self)
        return ffi.string(out).decode("ascii", "replace")

    def scan_isrc(self, lba: int) -> str | None:
        """Track ISRC, or ``None``. Start at the target track's first sector."""
        out = ffi.new("char[13]")
        rc = lib.accudisc_scan_isrc(self._handle, lba, out)
        if rc == lib.ACCUDISC_ERR_NOTFOUND:
            return None
        _check(rc, self)
        return ffi.string(out).decode("ascii", "replace")

    # -- reading -----------------------------------------------------------

    def read(
        self,
        lba: int,
        count: int,
        *,
        sink: SinkFn | None = None,
        copy: bool = True,
        c2: C2 | int = C2.NONE,
        sub: Sub | int = Sub.NONE,
        any_type: bool = False,
        retries: int = 0,
        chunk_sectors: int = 0,
        speed_x: int = 0,
        c2_retries: int = 0,
        verify_passes: int = 0,
        overlap_sectors: int = 0,
        speed_ladder: Sequence[int] | None = None,
        allow_unsafe: bool = False,
        status_map: bool = False,
        cancel: Cancel | None = None,
    ) -> ReadResult:
        """Stream ``count`` sectors from ``lba`` to ``sink``.

        ``sink`` may be ``None`` to read for status and stats only. It receives
        a :class:`Chunk`; return normally to continue, raise to cancel — the
        exception is re-raised here rather than swallowed.

        ``copy=False`` hands the sink a ``memoryview`` over library memory
        instead of ``bytes``, avoiding a copy of every sector. The view is
        released when your sink returns, and a view that escaped the call is
        detected and raises :class:`RetainedBufferError`. Use it when you
        consume the bytes inside the call; leave it alone otherwise.

        ``status_map=True`` allocates a ``count``-byte map; read it live from
        another thread through :attr:`ReadResult.status_map`.

        All the accuracy knobs default off, which is a single-pass fast read.
        Note ``speed_ladder`` applies only to problem-sector rereads, not to
        the streaming passes — for a speed-diverse sweep, run whole-range
        passes at different ``speed_x`` yourself.
        """
        if count <= 0:
            raise ValueError("count must be > 0")

        req = ffi.new("accudisc_read_req*")
        req.size = ffi.sizeof("accudisc_read_req")
        req.lba = lba
        req.count = count
        req.c2 = int(c2)
        req.sub = int(sub)
        req.any_type = 1 if any_type else 0
        req.retries = retries
        req.chunk_sectors = chunk_sectors
        req.speed_x = speed_x
        req.c2_retries = c2_retries
        req.verify_passes = verify_passes
        req.overlap_sectors = overlap_sectors
        req.allow_unsafe = 1 if allow_unsafe else 0

        keepalive: list = []
        if speed_ladder:
            rungs = ffi.new("uint16_t[]", list(speed_ladder))
            keepalive.append(rungs)
            req.speed_ladder = rungs
            req.ladder_len = len(speed_ladder)

        map_buf = None
        if status_map:
            map_buf = ffi.new("uint8_t[]", count)
            keepalive.append(map_buf)
            req.status_map = map_buf

        if cancel is not None:
            keepalive.append(cancel._flag)
            req.cancel = cancel._flag

        stats = ffi.new("accudisc_read_stats*")
        stats.size = ffi.sizeof("accudisc_read_stats")

        tramp = _SinkTrampoline(sink, copy) if sink is not None else None
        rc = lib.accudisc_read_cdda(
            self._handle, req,
            tramp.cffi_callback if tramp else ffi.NULL, ffi.NULL,
            stats,
        )
        # A sink that raised cancelled the read; report the CAUSE, not the
        # symptom, or the traceback says "cancelled" and loses the real error.
        if tramp is not None and tramp.error is not None:
            raise tramp.error
        _check(rc, self)

        return ReadResult(
            lba=lba,
            count=count,
            stats=_stats_from_c(stats),
            _map=map_buf,
            _keepalive=keepalive,
        )

    def read_span(
        self,
        lba: int,
        count: int,
        *,
        max_bytes: int = MAX_SPAN_BYTES,
        **kwargs,
    ) -> tuple[bytes, ReadResult]:
        """Read a bounded span and return ``(data, result)``.

        This is the call a subprocess transport cannot express: a span small
        enough to want in memory otherwise has to go out to a temp file and be
        read straight back — a full filesystem round-trip for bytes that only
        ever wanted to be in RAM, and on a recovery ladder that is
        ``passes x rungs`` round-trips per failed track.

        Refuses above ``max_bytes`` rather than silently allocating: use
        :meth:`read` with a sink for anything that large.
        """
        if count <= 0:
            raise ValueError("count must be > 0")
        sector_len = _sector_len(kwargs.get("c2", C2.NONE),
                                 kwargs.get("sub", Sub.NONE))
        want = count * sector_len
        if want > max_bytes:
            raise ValueError(
                f"span of {count} sectors is {want} bytes, over the "
                f"{max_bytes}-byte ceiling — use read() with a sink, or raise "
                f"max_bytes deliberately"
            )

        buf = bytearray(want)
        pos = 0

        def collect(chunk: Chunk) -> None:
            nonlocal pos
            n = chunk.nsec * chunk.sector_len
            buf[pos:pos + n] = chunk.data
            pos += n

        # copy=False is safe here: `collect` consumes the view synchronously
        # into `buf` and never stores it.
        result = self.read(lba, count, sink=collect, copy=False, **kwargs)
        return (bytes(buf[:pos]), result)

    def read_to_file(
        self,
        lba: int,
        count: int,
        *,
        pcm_path: str | os.PathLike | None = None,
        c2_path: str | os.PathLike | None = None,
        sub_path: str | os.PathLike | None = None,
        **kwargs,
    ) -> ReadResult:
        """Read a range, splitting the streams into separate files.

        Provided for completeness. For a **whole disc** the CLI's ``--pcm``
        path remains the better transport: it writes the file inside the
        library's address space, whereas this routes every sector through
        Python first. The cost is one memcpy per chunk against a multi-minute
        rip — small, but bought nothing, since the bytes land in the same file
        either way.
        """
        files: dict[str, object] = {}
        try:
            if pcm_path:
                files["pcm"] = open(pcm_path, "wb")
            if c2_path:
                files["c2"] = open(c2_path, "wb")
            if sub_path:
                files["sub"] = open(sub_path, "wb")
            if not files:
                raise ValueError("no output path given")

            def split(chunk: Chunk) -> None:
                for i in range(chunk.nsec):
                    base = i * chunk.sector_len
                    if "pcm" in files:
                        files["pcm"].write(
                            chunk.data[base:base + chunk.audio_len])
                    if "c2" in files and chunk.c2_len:
                        off = base + chunk.audio_len
                        files["c2"].write(chunk.data[off:off + chunk.c2_len])
                    if "sub" in files and chunk.sub_len:
                        off = base + chunk.audio_len + chunk.c2_len
                        files["sub"].write(chunk.data[off:off + chunk.sub_len])

            return self.read(lba, count, sink=split, copy=False, **kwargs)
        finally:
            for fh in files.values():
                fh.close()

    # -- pure TOC guards ---------------------------------------------------

    @staticmethod
    def default_audio_session(toc: Toc) -> int:
        """The session to rip when exactly one contains audio.

        Raises :class:`Unsupported` when more than one does — ambiguous by
        construction, so the caller must choose; iterating sessions is the
        calling application's business. Raises :class:`InvalidArgument` when
        session structure is unknown (the format-0 degrade path), which is
        deliberately NOT the same answer as "one session".
        """
        return _check(lib.accudisc_toc_default_audio_session(_toc_to_c(toc)))

    @staticmethod
    def session_range(toc: Toc, session: int) -> tuple[int, int]:
        """``(lba, count)`` for one session's tracks, excluding its lead-out."""
        lba = ffi.new("uint32_t*")
        count = ffi.new("uint32_t*")
        _check(lib.accudisc_toc_session_range(_toc_to_c(toc), session, lba, count))
        return (lba[0], count[0])

    @staticmethod
    def session_audio_range(toc: Toc, session: int) -> tuple[int, int]:
        """``(lba, count)`` for one session's AUDIO tracks."""
        lba = ffi.new("uint32_t*")
        count = ffi.new("uint32_t*")
        _check(lib.accudisc_toc_session_audio_range(
            _toc_to_c(toc), session, lba, count))
        return (lba[0], count[0])

    @staticmethod
    def track_range(toc: Toc, first: int, last: int) -> tuple[int, int]:
        """``(lba, count)`` spanning tracks ``first``..``last`` inclusive."""
        lba = ffi.new("uint32_t*")
        count = ffi.new("uint32_t*")
        _check(lib.accudisc_toc_track_range(_toc_to_c(toc), first, last,
                                            lba, count))
        return (lba[0], count[0])

    @staticmethod
    def check_audio_range(toc: Toc, lba: int, count: int) -> "RangeCheck":
        """Is ``[lba, lba+count)`` entirely audio within one session?

        Never raises for a non-rippable range: the verdict is the return value,
        carrying the reason and the first offending sector either way. Pure —
        no hardware access.
        """
        out = ffi.new("accudisc_range_check*")
        rc = lib.accudisc_check_audio_range(_toc_to_c(toc), lba, count, out)
        if rc == lib.ACCUDISC_ERR_INVAL:
            _raise(rc)
        return RangeCheck(
            ok=bool(out.ok),
            reason=ffi.string(
                lib.accudisc_range_reason_str(out.reason)).decode(),
            session=out.session,
            track=out.track,
            first_bad_lba=out.first_bad_lba,
        )


@dataclass(frozen=True, slots=True)
class RangeCheck:
    """Verdict from :meth:`Device.check_audio_range`."""

    ok: bool
    reason: str
    session: int
    track: int
    first_bad_lba: int


# ---------------------------------------------------------------------------
# internal conversions
# ---------------------------------------------------------------------------


def _sector_len(c2: C2 | int, sub: Sub | int) -> int:
    n = lib.ACCUDISC_BYTES_AUDIO
    if int(c2) == lib.ACCUDISC_C2_PTRS:
        n += lib.ACCUDISC_BYTES_C2
    elif int(c2) == lib.ACCUDISC_C2_PTRS_BEB:
        n += lib.ACCUDISC_BYTES_C2_BEB
    if int(sub) == lib.ACCUDISC_SUB_RAW:
        n += lib.ACCUDISC_BYTES_SUB_RAW
    elif int(sub) == lib.ACCUDISC_SUB_Q:
        n += lib.ACCUDISC_BYTES_SUB_Q
    return n


def _toc_from_c(c) -> Toc:
    tracks = tuple(
        Track(
            number=c.tracks[i].number,
            adr_ctrl=c.tracks[i].adr_ctrl,
            session=c.tracks[i].session,
            lba=c.tracks[i].lba,
            sectors=c.tracks[i].sectors,
            pregap=c.tracks[i].pregap,
        )
        for i in range(c.track_count)
    )
    sessions = tuple(
        Session(
            number=c.sessions[i].number,
            first_track=c.sessions[i].first_track,
            last_track=c.sessions[i].last_track,
            audio_tracks=c.sessions[i].audio_tracks,
            data_tracks=c.sessions[i].data_tracks,
            leadout_lba=c.sessions[i].leadout_lba,
        )
        for i in range(c.session_count)
    )
    return Toc(
        first_track=c.first_track,
        last_track=c.last_track,
        tracks=tracks,
        sessions=sessions,
        leadout_lba=c.leadout_lba,
        anomalies=Anomaly(c.anomalies),
        sessions_total=c.sessions_total,
    )


def _toc_to_c(toc: Toc):
    """Rebuild a C ``accudisc_toc`` so the pure guard functions can be used."""
    c = ffi.new("accudisc_toc*")
    c.first_track = toc.first_track
    c.last_track = toc.last_track
    c.track_count = len(toc.tracks)
    c.leadout_lba = toc.leadout_lba
    c.anomalies = int(toc.anomalies)
    c.sessions_total = toc.sessions_total
    c.session_count = len(toc.sessions)
    for i, t in enumerate(toc.tracks):
        c.tracks[i].number = t.number
        c.tracks[i].adr_ctrl = t.adr_ctrl
        c.tracks[i].session = t.session
        c.tracks[i].lba = t.lba
        c.tracks[i].sectors = t.sectors
        c.tracks[i].pregap = t.pregap
    for i, s in enumerate(toc.sessions):
        c.sessions[i].number = s.number
        c.sessions[i].first_track = s.first_track
        c.sessions[i].last_track = s.last_track
        c.sessions[i].audio_tracks = s.audio_tracks
        c.sessions[i].data_tracks = s.data_tracks
        c.sessions[i].leadout_lba = s.leadout_lba
    return c


def _stats_from_c(s) -> ReadStats:
    return ReadStats(
        sectors_read=s.sectors_read,
        sectors_flagged=s.sectors_flagged,
        c2_bits=s.c2_bits,
        hard_errors=s.hard_errors,
        max_bits_sector=s.max_bits_sector,
        first_flagged_lba=s.first_flagged_lba,
        last_flagged_lba=s.last_flagged_lba,
        sense_medium=s.sense_medium,
        sense_hardware=s.sense_hardware,
        sense_other=s.sense_other,
        rereads=s.rereads,
        sectors_recovered=s.sectors_recovered,
        sectors_suspect=s.sectors_suspect,
        slips=s.slips,
        subq_total=s.subq_total,
        subq_ok=s.subq_ok,
    )
