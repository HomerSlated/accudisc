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
from typing import Callable, Iterable, Sequence

from ._accudisc import ffi, lib

__all__ = [
    "AccuDiscError", "InvalidArgument", "OutOfMemory", "OpenFailed", "IOFailed",
    "SenseError", "ShortResponse", "Unsupported", "NotBlank", "Cancelled", "CrcError",
    "NotFound", "UnsafeCombination", "AbiMismatch", "RetainedBufferError",
    "C2", "Sub", "MapState", "Anomaly", "TocSource", "TocDegrade", "C2Verdict",
    "Verdict", "WriteResult",
    "Sense", "DriveId", "Track", "Session", "Toc", "TocInfo", "Features",
    "SpeedRung", "C2Lag", "Chunk", "ReadStats", "ReadResult", "Cancel", "Device", "Q",
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
    """The loaded library's version as it prints it, e.g. ``"0.4.0"``."""
    return ffi.string(lib.accudisc_version_string()).decode()


def _check_version_skew() -> None:
    """Refuse a binding compiled against a different library than it loaded.

    Compares all three components. It used to compare ``[:2]``, on the reasoning
    that patch releases do not change layout — true as a rule, and worthless as
    a guard, because the rule it depends on is one WE have to keep. cdda2img
    (§113.2) found the case: three struct layouts changed inside 0.2.0 without a
    version bump, so the check compared 0.2 to 0.2 and passed a two-day-old
    extension against a freshly built library.

    Full-tuple comparison costs a rebuild on a patch bump, which is cheap. The
    false negative it removes is not: :class:`AbiMismatch` is the one error
    cdda2img's transport treats as "degrade to the subprocess", so a miss here
    does not surface as a failure, it surfaces as well-formed calls about the
    wrong bytes.

    This is a backstop, never the primary defence — that is the per-struct
    ``size`` field, which holds regardless of what the version says.
    """
    compiled, loaded = version, library_version()
    if compiled != loaded:
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


class NotBlank(AccuDiscError):
    """``ACCUDISC_ERR_NOT_BLANK`` — the disc is not blank; nothing was written.

    Its own class since 0.4.0. Before that this arrived as
    :class:`Unsupported`, which was *exact* — the library had exactly one
    reachable ``ERR_UNSUPPORTED`` under the write path — but exact **by
    census, not by construction**. Any new ``ERR_UNSUPPORTED`` there would have
    silently joined the meaning, and the resulting bug reads as correct at both
    ends: the caller tells the user to insert a blank disc they are already
    holding, and no test on either side can tell.

    Catch this rather than string-matching a message, and note it is
    **narrower** than the token it replaces: `Unsupported` on `write()` now
    means what it says everywhere else.
    """


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
    lib.ACCUDISC_ERR_NOT_BLANK: NotBlank,
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


class WriteResult(enum.Enum):
    """The outcome of a burn that COMPLETED.

    Both members mean the disc was written. There is no member for a failure:
    a burn that did not complete raises, because the one mistake this API
    exists to prevent is a caller reporting a written disc as blank.

    :attr:`token` reproduces the CLI's ``summary … result=`` value, which is
    what cdda2img keys decisions on. The CLI's four tokens do not all live
    here, because two of them are not outcomes of a completed burn — the full
    mapping, which is the whole contract:

    ==================  ===================================================
    CLI ``result=``     binding
    ==================  ===================================================
    ``ok``              :attr:`WriteResult.OK`
    ``caveats``         :attr:`WriteResult.CAVEATS`
    ``not_blank``       raises :class:`NotBlank` — nothing was written
    ``error``           raises another :class:`AccuDiscError`
    ==================  ===================================================

    Exit codes are deliberately absent. They are a *process* convention that
    stays in the CLI (API_PLAN §3); a binding reproduces the semantics, not
    the mechanism.
    """

    OK = "ok"
    CAVEATS = "caveats"

    @property
    def token(self) -> str:
        return self.value

    @property
    def clean(self) -> bool:
        """``False`` for CAVEATS. The disc was still written either way."""
        return self is WriteResult.OK


class Verdict(enum.IntEnum):
    """Is a page-2A speed setting a REAL rung of this drive's ladder?

    Report-only. The library never rewrites a caller's ``speed_ladder`` from
    this, and neither does the binding.

    ``UNKNOWN`` is not a failure — it is what every rung gets when
    ``points == 1``, because a verdict needs an interval and point samples
    cannot supply one. Do not treat it as "no ladder"; treat it as "you did
    not ask for one".
    """

    UNKNOWN = lib.ACCUDISC_RUNG_UNKNOWN
    ADMITTED = lib.ACCUDISC_RUNG_ADMITTED
    DUPLICATE = lib.ACCUDISC_RUNG_DUPLICATE
    QUANTIZED = lib.ACCUDISC_RUNG_QUANTIZED

    @property
    def token(self) -> str:
        """The token the CLI prints for this verdict (``verdict=admitted``).

        ``UNKNOWN`` prints nothing at all on the CLI, so it has no token and
        this returns ``""`` — matching absence, not inventing a word for it.
        """
        return {
            Verdict.ADMITTED: "admitted",
            Verdict.DUPLICATE: "duplicate",
            Verdict.QUANTIZED: "quantized",
        }.get(self, "")


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
class SpeedRung:
    """One candidate speed setting, and what it actually delivered.

    Rates are in **x** (float), converted from the library's centi-x. The
    C struct's integer centi-x is preserved in the ``*_cx`` fields for anyone
    who wants exact comparisons without float equality.

    ``min_x``/``max_x`` are ``None`` when no gradient was measured — which is
    every rung at ``points == 1``, and a rung that failed to measure at any
    ``points``. This is the whole reason they are not plain floats: the C
    struct signals "not measured" with a 0, and a 0.00 handed to a caller
    reads as a rung that stalled. The CLI solves the same problem by omitting
    the ``min=``/``max=`` tokens entirely.
    """

    requested_x: int
    reported_x: int          #: page 2A after the set; 0 = not available
    measured_cx: int         #: centi-x at ONE radius (the MIDDLE band if points == 3)
    min_cx: int | None
    max_cx: int | None
    equiv_x: int             #: for DUPLICATE/QUANTIZED, the rung this collapses onto
    verdict: Verdict

    @property
    def measured_x(self) -> float:
        return self.measured_cx / 100.0

    @property
    def min_x(self) -> float | None:
        return None if self.min_cx is None else self.min_cx / 100.0

    @property
    def max_x(self) -> float | None:
        return None if self.max_cx is None else self.max_cx / 100.0

    @property
    def spread_cx(self) -> int | None:
        """``max_cx - min_cx`` — the rate change across the whole probed span.

        ``None`` when no gradient was measured. This is the quantity the
        admission rule calibrates against: it is the radius term measured on
        THIS disc, so a flat rung is either CLV-clamped or not being measured
        properly, and cross-rung rate comparisons are only meaningful once
        discounted by it.
        """
        if self.min_cx is None or self.max_cx is None:
            return None
        return self.max_cx - self.min_cx


@dataclass(frozen=True, slots=True)
class C2Lag:
    """The drive's C2-bitmap/audio alignment, and the evidence behind it.

    ``lag_pairs`` is ``None`` when the probe could not conclude — which is a
    NORMAL outcome, not an error, and is why this returns a value rather than
    raising. A clean disc, a clean span, or flags incoherent with reread
    instability all land here. The evidence fields are still filled in, so the
    two cases the library distinguishes stay distinguishable:

    * ``sectors_active == 0`` — no C2 fired anywhere in the span. Nothing was
      wrong with the probe; there was nothing to measure. Try a damaged span,
      or a speed at which flags actually fire.
    * ``sectors_active > 0`` with ``lag_pairs is None`` — C2 fired, but the
      reread evidence was too thin to pick a peak. A larger span may conclude.

    :attr:`c2_fired` names that split so a caller does not re-derive it.

    **Sign convention:** a fired bit at bitmap position *i* describes audio byte
    ``i - 4 * lag_pairs``. Positive lag means the bitmap trails the audio.

    **Report-only.** AccuDisc never applies this to the bitmaps it delivers —
    it is a factual property of the drive for the caller to record and apply.
    Anything treating fired bits as byte-exact damage positions (an erasure
    feed for parity repair) must correct for it; misplaced erasures actively
    harm decoding, so an unapplied lag is worse than none.

    ``peak_milli`` is agreement against a PROXY oracle — reread instability,
    which cannot see bytes that fail identically in paired reads — so expect it
    well below what a database oracle would give. It is not a confidence score
    to threshold on: a verdict is only returned at all when the peak dominates
    every other shift, so any non-``None`` ``lag_pairs`` is already unambiguous.
    """

    lag_pairs: int | None    #: None = inconclusive; see the class docstring
    sectors_active: int      #: C2-active sectors seen in the scan pass
    flags_used: int          #: fired C2 bits contributing at the peak
    diff_bytes: int          #: unstable byte observations accumulated
    peak_milli: int          #: flags landing on unstable bytes at the peak, ‰
    runner_milli: int        #: best agreement at any OTHER shift, ‰

    @property
    def conclusive(self) -> bool:
        """Did the probe return an alignment? Equivalent to ``lag_pairs is not None``."""
        return self.lag_pairs is not None

    @property
    def c2_fired(self) -> bool:
        """Did any C2 fire in the span at all?

        The one distinction worth making on an inconclusive result: ``False``
        means the span was clean and the probe had nothing to work with,
        ``True`` means it had evidence and it was not enough. They call for
        different next moves, and telling them apart from ``lag_pairs`` alone
        is impossible.
        """
        return self.sectors_active > 0


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

    def probe_c2_lag(self, lba: int = 0, count: int | None = None) -> C2Lag:
        """Measure this drive's C2-bitmap/audio alignment. **Needs DAMAGED media.**

        Some drives return the C2 bitmap shifted from the audio bytes of the
        same sector by a small constant amount (2 sample pairs on the Plextor
        PX-716A). The probe needs no external reference: fired flags mark bytes
        the CIRC decoder failed on, failed bytes are unstable across
        cache-defeated rereads, so flag positions are cross-correlated against
        reread instability over candidate shifts and the agreement peak is the
        lag.

        That method is why the media matters. A clean disc fires no flags and
        there is nothing to correlate — so this returns an INCONCLUSIVE
        :class:`C2Lag` (``lag_pairs is None``) rather than raising. Absence of
        evidence is not an I/O failure, and it is common enough that making it
        an exception would put a ``try`` around the normal case.

        ``count`` defaults to the rest of the disc from ``lba``, matching
        ``accudisc c2lag``. A wider span gives the correlation more to work
        with; the probe scans for C2-active sectors and only rereads those, so
        span costs less than it looks.

        Speed is not set here. Flags fire more readily at higher speeds, so if
        a span comes back with ``c2_fired`` false, :meth:`set_speed` upward and
        retry before concluding the disc is clean. The drive is left wherever
        the caller put it.

        The result is a property of the DRIVE, not the disc, so it is worth
        caching per drive — but it is **report-only**: the library goes on
        delivering raw bitmaps, and applying the correction is the caller's
        job. See :class:`C2Lag`.
        """
        if count is None:
            leadout = self.read_toc().leadout_lba
            if lba >= leadout:
                raise InvalidArgument(
                    lib.ACCUDISC_ERR_INVAL,
                    f"lba {lba} is at or past lead-out {leadout}",
                )
            count = leadout - lba

        out = ffi.new("accudisc_c2_lag*")
        rc = lib.accudisc_probe_c2_lag(self._handle, lba, count, out)
        if rc == lib.ACCUDISC_ERR_NOTFOUND:
            return _c2_lag_from_c(out[0], conclusive=False)
        _check(rc, self)
        return _c2_lag_from_c(out[0], conclusive=True)

    #: The CLI's default candidate ladder, before the page-2A cap is applied
    #: (``cli/main.c`` ``cmd_speeds``). Fastest-first, so the returned rows and
    #: :meth:`admitted_ladder` come back ready to be stepped down.
    DEFAULT_LADDER = (52, 48, 40, 32, 24, 16, 8, 4)

    def probe_speed_ladder(
        self,
        *,
        points: int = 3,
        lba: int | None = None,
        count: int | None = None,
        candidates: Sequence[int] | None = None,
    ) -> tuple[SpeedRung, ...]:
        """Time each candidate speed and report which are real rungs.

        Page 2A reports the SETTING, not reality: a drive will accept a speed
        it cannot deliver, and will deliver the same rate for two different
        settings. Only a timed streaming read can tell, so this sets each
        candidate, lets the drive settle, and times a read in a window
        disjoint from every other window of every other rung — the cache can
        never serve a remeasure.

        ``points``:

        * ``3`` (default) cuts the span into three equal bands and measures
          each rung once in each, giving it a RANGE. Only at ``points == 3``
          does any rung get a :class:`Verdict` other than ``UNKNOWN``.
        * ``1`` (or ``0``, which the library normalises to 1) measures one
          window per rung. Every verdict comes back ``UNKNOWN`` — deliberately,
          because a ladder derived from point samples is a confident wrong
          answer.

        **Any other value is** :class:`InvalidArgument` **— it is not rounded
        down.** ``points=2`` is a request the library cannot honour, not a
        request for 1.

        ``lba``/``count`` default to the same span the CLI chooses, so rows
        stay comparable with every ``accudisc speeds`` measurement without the
        caller re-deriving it. Note the two are NOT the same span, which is
        the part worth reading: at ``points > 1`` the bands are the point, so
        the span opens out to the WHOLE disc to make them inner/middle/outer;
        at ``points == 1`` it is the middle half, one representative radius.
        Passing ``lba`` yourself narrows the span, and three bands of the last
        sixth of a disc are still three well-formed numbers with nothing to
        mark them as local.

        ``candidates`` defaults to :attr:`DEFAULT_LADDER` capped at the drive's
        page-2A maximum.

        Every candidate is returned, in the order given — this marks rungs,
        it never discards them. **The drive is LEFT at the last candidate
        tested**; speed is not auto-restored, exactly as with reads.

        The verdicts are about THIS disc, not this drive. A rung admitted on a
        short disc may be unreachable on a longer one, and media whose rate
        falls off toward the outer edge — observed, not hypothetical — can
        invalidate a rung admitted mid-disc. Probe per disc; never cache a
        ladder across discs.
        """
        if points not in (0, 1, 3):
            raise InvalidArgument(
                lib.ACCUDISC_ERR_INVAL,
                f"points must be 1 or 3 (0 means 1), not {points} — the library "
                "refuses anything else rather than rounding it down",
            )

        if candidates is None:
            try:
                max_kbps, _ = self.get_speed()
                max_x = max_kbps // 176
            except AccuDiscError:
                max_x = 52
            cand = tuple(x for x in self.DEFAULT_LADDER if x <= max_x)
            if not cand:
                cand = (max_x or 1,)
        else:
            cand = tuple(int(x) for x in candidates)
            if not cand:
                raise InvalidArgument(lib.ACCUDISC_ERR_INVAL,
                                      "candidates is empty")
        if len(cand) > 255:
            raise InvalidArgument(lib.ACCUDISC_ERR_INVAL,
                                  "at most 255 candidates (ncand is a uint8_t)")

        if lba is None or count is None:
            leadout = self.read_toc().leadout_lba
            if lba is None:
                lba = 0 if points > 1 else leadout // 4
            if count is None:
                count = leadout - lba if leadout > lba else 0
                if points <= 1 and count > leadout // 2:
                    count = leadout // 2

        c_cand = ffi.new("uint16_t[]", list(cand))
        out = ffi.new("accudisc_speed_rung[]", len(cand))
        _check(
            lib.accudisc_probe_speed_ladder(
                self._handle, lba, count, c_cand, len(cand), points, out
            ),
            self,
        )
        return tuple(_rung_from_c(out[i]) for i in range(len(cand)))

    # -- recording ---------------------------------------------------------

    def write(
        self,
        toc_path: str,
        bin_path: str,
        *,
        simulate: bool = False,
        byteswap: bool = False,
        speed: int = 0,
        cdtext_path: str | None = None,
        progress: Callable[[int, int], None] | None = None,
    ) -> WriteResult:
        """Burn one audio session Disc-At-Once. **This destroys a blank disc.**

        ``toc_path`` is a cdrdao ``.toc``; ``bin_path`` is the raw s16 audio it
        names. ``cdtext_path`` is a raw READ TOC format-0x05 blob, byte-for-byte
        as :meth:`read_cdtext_raw` emits it, laid into the lead-in verbatim.

        Requires a blank disc and a device opened with ``rdwr=True``.

        Returns :class:`WriteResult` — and read what that class says about the
        four CLI tokens, because the split is the contract. In short: a return
        means the disc **was written**, an exception means it was not.

        :class:`WriteResult.CAVEATS` is the case worth designing around. The
        burn completed, but something was logged that the caller must surface
        — today, a CD-Text SIZE_INFO pack whose declared track range disagrees
        with the ``.toc``. The detail arrives only through
        :meth:`set_log`, so install a log sink before burning or the caveat is
        a boolean with no explanation.

        ``progress(done, total)`` is called with sector counts. An exception
        raised inside it propagates out of this call, but **does not stop the
        burn** — the C progress callback returns void, so there is nothing to
        signal cancellation with. Do not put work that can fail in it.

        ``speed=0`` leaves the drive's current write speed.
        """
        opts = ffi.new("accudisc_write_opts*")
        opts.size = ffi.sizeof("accudisc_write_opts")
        opts.simulate = 1 if simulate else 0
        opts.byteswap = 1 if byteswap else 0
        opts.speed = int(speed)
        # Kept alive for the duration of the call: cffi frees an unreferenced
        # new_allocator result, and opts.cdtext_path would then dangle into a
        # burn. Assigning it to a local is load-bearing, not tidiness.
        keep_cdtext = ffi.NULL
        if cdtext_path is not None:
            keep_cdtext = ffi.new("char[]", str(cdtext_path).encode())
        opts.cdtext_path = keep_cdtext

        raised: list[BaseException] = []

        if progress is None:
            cb = ffi.NULL
        else:
            @ffi.callback("void(void *, uint32_t, uint32_t)",
                          onerror=lambda exc, val, tb: raised.append(exc))
            def cb(_user, done, total):
                progress(done, total)

        rc = lib.accudisc_write(
            self._handle,
            str(toc_path).encode(),
            str(bin_path).encode(),
            opts,
            cb,
            ffi.NULL,
        )

        # Order matters. A progress callback that raised must not mask the
        # outcome of a burn that COMPLETED -- reporting a written disc as
        # failed is the exact error this API is shaped to prevent. So the
        # library's verdict is resolved first and the callback exception is
        # only re-raised when the burn did not complete anyway.
        if rc < 0:
            if raised:
                raise raised[0]
            _raise(rc, self)
        if raised:
            import warnings
            warnings.warn(
                f"progress callback raised {raised[0]!r} — the burn COMPLETED "
                f"and the disc is written; the exception is not being raised "
                f"because that would read as a failed burn",
                RuntimeWarning,
                stacklevel=2,
            )
        return (WriteResult.CAVEATS if rc == lib.ACCUDISC_WROTE_WITH_CAVEATS
                else WriteResult.OK)

    @staticmethod
    def admitted_ladder(rungs: Iterable[SpeedRung]) -> tuple[int, ...]:
        """The requested speeds of the ADMITTED rungs, in the order probed.

        The equivalent of the CLI's ``ladder admitted=`` line, and it carries
        the same caveat: an EMPTY tuple is not the same as no ladder. At
        ``points == 1`` every verdict is ``UNKNOWN`` and this is empty because
        nothing was judged — the CLI expresses that by printing no line at
        all, which a parser can distinguish and a ``()`` cannot. If you need
        to tell those apart, test the verdicts.
        """
        return tuple(r.requested_x for r in rungs
                     if r.verdict == Verdict.ADMITTED)

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
            # _sector_len() is our PREDICTION of a number the library REPORTS.
            # Slice assignment into a bytearray silently resizes it, so a wrong
            # prediction would yield a plausible buffer of the wrong length
            # rather than an error. Check it instead of trusting it.
            if chunk.sector_len != sector_len:
                raise AccuDiscError(
                    lib.ACCUDISC_ERR_INVAL,
                    f"sector_len mismatch: predicted {sector_len}, library "
                    f"delivered {chunk.sector_len} — the c2/sub layout "
                    f"assumption is wrong, refusing to reassemble the span"
                )
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

        **Use it for a whole disc. It costs nothing measurable.** This docstring
        used to steer callers to the CLI's ``--pcm`` instead, on two claims that
        were both wrong:

        * *"the CLI writes the file inside the library's address space."* It
          does not. ``read_sink()`` (``cli/main.c:1000``) also loops per sector
          and ``fwrite``\\ s — the library never writes the file on either path.
          The real difference is a C callback versus a Python one per chunk.
        * *"the cost is small but bought nothing."* Small was a guess, and the
          advice outlived the audience: the only party reading this is a
          binding consumer, who has been told not to shell out.

        Measured instead, by cdda2img, as A/B/A over a whole disc (Tracy,
        162892 sectors, req=40, pcm + c2 + raw sub on both arms):

        =========================  ==========  =============
        arm                        wall clock  rate
        =========================  ==========  =============
        A1 subprocess              116.43 s    18.65x
        B  **binding**             112.69 s    19.27x
        A2 subprocess              112.75 s    19.26x
        =========================  ==========  =============

        The binding differs from the adjacent subprocess arm by **0.06 s over
        112 s**, against a subprocess-vs-subprocess noise floor of **3.68 s**.
        It lands on the faster side, which is a rounding error with a sign
        rather than a result. PCM and C2 came back byte-identical to the
        adjacent arm over 383 MB and 48 MB respectively.

        So the Python sink is not the bottleneck at 40x — the drive is — and
        there is no library-side whole-disc entry point, deliberately. One
        drive, one disc, one speed; it is not a claim about a 48x drive on a
        clean pressing.

        ``sub`` does not reproduce run to run and cannot be compared: no CIRC
        protection, a per-frame CRC-16 as its only check, failing independently
        of the audio counters.
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


def _rung_from_c(r) -> SpeedRung:
    """One C rung row.

    The 0 -> None mapping on min/max is the only translation here that is not
    a straight copy, and it is the point of the wrapper: the C struct uses 0
    for "no gradient was measured", which as a plain number is indistinguishable
    from a rung that measured zero. Both fields are set together or not at all
    (the engine only fills them when EVERY band measured), so testing either
    is equivalent — `or` rather than `and` so a future half-filled row surfaces
    as None instead of a bare 0.
    """
    measured = r.min_cx or r.max_cx
    return SpeedRung(
        requested_x=r.requested_x,
        reported_x=r.reported_x,
        measured_cx=r.measured_cx,
        min_cx=r.min_cx if measured else None,
        max_cx=r.max_cx if measured else None,
        equiv_x=r.equiv_x,
        verdict=Verdict(r.verdict),
    )


def _c2_lag_from_c(c, conclusive: bool) -> C2Lag:
    """One C lag struct.

    ``conclusive`` comes from the RETURN CODE, never from inspecting the
    struct. On ACCUDISC_ERR_NOTFOUND the library still fills every evidence
    field, and `lag_pairs` is then whatever shift happened to score highest —
    a well-formed integer that means nothing. There is no value of it that
    signals "inconclusive", so the only thing that can carry that fact is the
    rc, and it has to be passed in rather than re-derived here.
    """
    return C2Lag(
        lag_pairs=c.lag_pairs if conclusive else None,
        sectors_active=c.sectors_active,
        flags_used=c.flags_used,
        diff_bytes=c.diff_bytes,
        peak_milli=c.peak_milli,
        runner_milli=c.runner_milli,
    )


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
