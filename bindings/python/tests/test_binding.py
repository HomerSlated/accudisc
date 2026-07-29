"""Device-free tests for the AccuDisc Python binding.

Everything here runs with no drive, no disc and no privilege, which is what
makes it usable as a build gate. Two things make that possible:

* The **ABI layer is reachable without a device.** An ``ERR_INVAL`` from
  ``accudisc_read_cdda(NULL, &req, ...)`` is positive evidence that the size
  checks accepted the struct and execution got past them; an ``ERR_ABI`` is
  evidence they rejected it. So every rule in API_PLAN §7.1 is testable here.
* The **sink callback is callable from Python.** A cffi callback object can be
  invoked directly, so the trampoline's exception handling and buffer-lifetime
  enforcement can be exercised without the engine ever running.

Runs under pytest, or standalone (``python3 tests/test_binding.py``) so the
CMake/ctest gate does not need pytest installed.
"""

from __future__ import annotations

import sys

import accudisc as ad
from accudisc._accudisc import ffi, lib


# ---------------------------------------------------------------------------
# version and constants
# ---------------------------------------------------------------------------


def test_imported_package_is_the_binding_not_a_namespace_phantom():
    """`import accudisc` can succeed and yield something that is not us.

    A *directory* named `accudisc` anywhere on sys.path is recorded as a PEP 420
    namespace portion: the import completes, `__file__` is None, and there is no
    `Device`. Nothing raises, so `except ImportError` cannot see it and the
    failure surfaces at the first attribute access, arbitrarily far away.

    The case that bites is our own repo root — measured, from its parent:

        $ cd ~/Git && python3 -c "import accudisc; print(accudisc)"
        <module 'accudisc' (namespace) from ['/home/kgr/Git/accudisc']>

    A real package later on the path does win the scan (also measured), which is
    why this passes here and why the hazard is invisible until it isn't. Found
    by cdda2img (§104.1), whose harness had reasoned the phantom harmless.
    """
    assert ad.__file__ is not None, (
        "imported a PEP 420 namespace portion, not the binding: "
        f"__path__={list(getattr(ad, '__path__', []))}")
    for name in ("Device", "AccuDiscError", "AbiMismatch"):
        assert hasattr(ad, name), f"module lacks {name}; not the binding"


def test_version_agrees_with_loaded_library():
    """Header macros vs the .so. CMake single-sources these; a mismatch is skew."""
    assert ad.version == ad.library_version()
    assert ad.version_string() == ".".join(str(n) for n in ad.library_version())


def test_skew_check_sees_a_PATCH_level_difference():
    """The case the old ``[:2]`` comparison could not see.

    cdda2img (§113.2) found it live: three struct layouts changed inside 0.2.0
    with no version bump, so the guard compared 0.2 to 0.2 and passed a
    two-day-old extension against a freshly built library. Patch-level skew is
    now a refusal.

    Driven by faking the LOADED version, because the honest pair cannot occur in
    one build tree — which is precisely why it went unnoticed.

    The fakes are DERIVED from the real version, one component at a time, not
    written as literals. Literals were the first version of this test and they
    broke on the very next bump: `(0, 4, 0)` was a fake under 0.3.0 and became
    the truth under 0.4.0, so the case silently stopped testing anything and
    the failure it produced pointed at the guard rather than at itself.
    """
    real = ad.library_version
    maj, minr, pat = ad.version
    for fake in ((maj, minr, pat + 1), (maj, minr + 1, pat), (maj + 1, minr, pat)):
        ad.library_version = lambda f=fake: f
        try:
            ad._check_version_skew()
        except ad.AbiMismatch as exc:
            assert "rebuild" in str(exc)
        else:
            raise AssertionError(f"loaded {fake} vs compiled {ad.version} passed")
        finally:
            ad.library_version = real

    # The complement: the honest pair must still be accepted, or the guard is
    # just a refusal with a version number in it.
    ad._check_version_skew()


def test_package_version_matches_the_library():
    """pyproject's version is hand-kept; this is what makes that safe.

    A binding whose declared version can drift from the ABI it was built
    against is a pin nobody can rely on — and cdda2img's stated plan is to key
    on exactly this number.
    """
    from pathlib import Path

    pyproject = Path(__file__).resolve().parent.parent / "pyproject.toml"
    text = pyproject.read_text()
    try:
        import tomllib
        declared = tomllib.loads(text)["project"]["version"]
    except ModuleNotFoundError:
        # tomllib is 3.11+, and this package's declared floor is 3.10. Skipping
        # would be worse than a crude parse: this test is the only thing
        # standing between a hand-kept version and a pin cdda2img relies on,
        # and it would be silently absent on exactly the interpreter they ship
        # on. One regex, anchored to the [project] table's own field.
        import re
        m = re.search(r'(?m)^version\s*=\s*"([^"]+)"', text)
        assert m, "no version field in pyproject.toml"
        declared = m.group(1)
    assert declared == ad.version_string(), (
        f"pyproject.toml says {declared}, libaccudisc says {ad.version_string()}")


def test_struct_sizes_match_api_plan():
    """The measured sizes API_PLAN §7.1 recorded, pinned here too.

    Not a tautology despite API mode: these numbers are the ones the growth
    table and the ABI rules were reasoned about, so a silent change should
    fail something.
    """
    assert ffi.sizeof("accudisc_read_req") == 56
    assert ffi.sizeof("accudisc_read_stats") == 136
    assert ffi.sizeof("accudisc_chunk") == 32


def test_error_codes_are_the_headers():
    assert lib.ACCUDISC_ERR_INVAL == -1
    assert lib.ACCUDISC_ERR_NOTFOUND == -10
    assert lib.ACCUDISC_ERR_UNSAFE_COMBINATION == -11
    assert lib.ACCUDISC_ERR_ABI == -12
    for code, cls in ad._ERRORS.items():
        exc = cls(code)
        assert exc.code == code
        assert exc.name  # accudisc_strerror never returns NULL


def test_abi_error_is_its_own_type():
    """A caller must be able to catch 'rebuild' without catching 'bad args'."""
    assert issubclass(ad.AbiMismatch, ad.AccuDiscError)
    assert not issubclass(ad.AbiMismatch, ad.InvalidArgument)
    assert not issubclass(ad.InvalidArgument, ad.AbiMismatch)


# ---------------------------------------------------------------------------
# the ABI size rules (API_PLAN §7.1), device-free
# ---------------------------------------------------------------------------


def _call_with_req_size(size: int, *, alloc: int | None = None,
                        tail_nonzero: bool = False) -> int:
    n = alloc if alloc is not None else max(size, ffi.sizeof("accudisc_read_req"))
    buf = ffi.new("char[]", n)
    req = ffi.cast("accudisc_read_req*", buf)
    req.size = size
    req.count = 1
    if tail_nonzero:
        ffi.cast("unsigned char*", buf)[ffi.sizeof("accudisc_read_req")] = 1
    return lib.accudisc_read_cdda(ffi.NULL, req, ffi.NULL, ffi.NULL, ffi.NULL)


def test_req_exact_size_is_accepted():
    """ERR_INVAL (not ERR_ABI) proves the ABI layer passed and the NULL dev hit."""
    assert _call_with_req_size(ffi.sizeof("accudisc_read_req")) == lib.ACCUDISC_ERR_INVAL


def test_req_zero_size_is_refused():
    """What a caller who forgot the INIT macro produces — must fail loudly."""
    assert _call_with_req_size(0) == lib.ACCUDISC_ERR_ABI


def test_req_short_size_is_zero_extended():
    """IN direction: an older caller gets older behaviour, not a refusal."""
    short = ffi.sizeof("accudisc_read_req") - 8
    assert _call_with_req_size(short) == lib.ACCUDISC_ERR_INVAL


def test_req_long_size_with_zero_tail_is_accepted():
    n = ffi.sizeof("accudisc_read_req") + 8
    assert _call_with_req_size(n, alloc=n) == lib.ACCUDISC_ERR_INVAL


def test_req_long_size_with_set_tail_is_refused():
    """A set byte past our end = a feature this build does not have."""
    n = ffi.sizeof("accudisc_read_req") + 8
    assert _call_with_req_size(n, alloc=n, tail_nonzero=True) == lib.ACCUDISC_ERR_ABI


def test_req_absurd_size_is_bounded_not_scanned():
    """ADSC_ABI_GROWTH_MAX: the guard must not run off the end of the struct.

    ``size`` is the caller's claim, straight out of caller memory, so an
    uninitialised struct routinely puts something huge there. The first version
    of this guard had exactly the bug it existed to prevent.
    """
    assert _call_with_req_size(1 << 30, alloc=ffi.sizeof("accudisc_read_req")) \
        == lib.ACCUDISC_ERR_ABI


def test_stats_long_size_is_refused():
    """OUT direction: refusing beats leaving believed-in counters unfilled."""
    n = ffi.sizeof("accudisc_read_stats") + 8
    buf = ffi.new("char[]", n)
    stats = ffi.cast("accudisc_read_stats*", buf)
    stats.size = n
    req = ffi.new("accudisc_read_req*")
    req.size = ffi.sizeof("accudisc_read_req")
    req.count = 1
    assert lib.accudisc_read_cdda(ffi.NULL, req, ffi.NULL, ffi.NULL, stats) \
        == lib.ACCUDISC_ERR_ABI


# ---------------------------------------------------------------------------
# the sink trampoline
# ---------------------------------------------------------------------------


def _fake_chunk(nsec: int = 2, sector_len: int = 2352):
    """A chunk pointing at Python-owned memory, for calling the callback direct."""
    data = ffi.new("uint8_t[]", nsec * sector_len)
    for i in range(nsec * sector_len):
        data[i] = i & 0xFF
    c = ffi.new("accudisc_chunk*")
    c.lba = 1000
    c.nsec = nsec
    c.data = data
    c.sector_len = sector_len
    c.audio_len = sector_len
    c.c2_len = 0
    c.sub_len = 0
    return c, data  # keep `data` alive


def test_sink_exception_cancels_rather_than_continuing():
    """The hazard: cffi's default callback returns 0, which means CONTINUE.

    A sink that raised would otherwise leave the rip running and the error
    visible only as a traceback on stderr.
    """
    def boom(chunk):
        raise ValueError("sink said no")

    tramp = ad._SinkTrampoline(boom, copy=True)
    c, _keep = _fake_chunk()
    rc = tramp.cffi_callback(ffi.NULL, c)
    assert rc != 0, "a raising sink must cancel the read"
    assert isinstance(tramp.error, ValueError)
    assert str(tramp.error) == "sink said no"


def test_sink_copy_mode_hands_over_owned_bytes():
    seen = []
    tramp = ad._SinkTrampoline(lambda ch: seen.append(ch), copy=True)
    c, _keep = _fake_chunk()
    assert tramp.cffi_callback(ffi.NULL, c) == 0
    assert tramp.error is None
    chunk = seen[0]
    assert isinstance(chunk.data, bytes)
    assert len(chunk.data) == chunk.nsec * chunk.sector_len
    assert chunk.lba == 1000
    assert chunk.audio(0)[:4] == b"\x00\x01\x02\x03"


def test_sink_zero_copy_view_is_released_on_return():
    """A stored view must raise, not read freed memory as plausible PCM."""
    stash = []
    tramp = ad._SinkTrampoline(lambda ch: stash.append(ch.data), copy=False)
    c, _keep = _fake_chunk()
    rc = tramp.cffi_callback(ffi.NULL, c)
    assert rc == 0 and tramp.error is None
    try:
        bytes(stash[0][:4])
    except ValueError as exc:
        assert "released" in str(exc)
    else:
        raise AssertionError("a retained view must not still be readable")


def test_sink_zero_copy_retained_slice_is_a_KNOWN_HOLE():
    """Pins the limit of the guard, so it is a measurement and not a hope.

    A ``memoryview`` slice re-exports from the underlying buffer rather than
    from the parent view, so releasing what we handed out does not reach it.
    This test asserts the hole EXISTS — if a future CPython closes it, this
    fails and the docs get to become stronger. It is why ``copy=True`` is the
    default and zero-copy is opt-in.
    """
    stash = []
    tramp = ad._SinkTrampoline(lambda ch: stash.append(ch.data[8:16]), copy=False)
    c, keep = _fake_chunk()
    tramp.cffi_callback(ffi.NULL, c)
    # keep the C buffer alive so reading the escaped slice is not a use-after-free
    assert keep is not None
    assert bytes(stash[0]) == bytes(range(8, 16)), (
        "if this fails, a slice no longer survives the parent's release — "
        "good news, tighten the guard")


def test_read_to_file_slicing_pattern_is_not_flagged():
    """read_to_file slices the zero-copy view — assert it does not self-trip.

    Its sink writes ``chunk.data[base:base+audio_len]`` per sector, which is a
    memoryview slice handed to a buffered writer. If the writer still held one
    when the view is released, read_to_file would raise on its own code path
    and be broken outright.
    """
    import io

    out = io.BytesIO()

    def slicing_sink(ch):
        for i in range(ch.nsec):
            base = i * ch.sector_len
            out.write(ch.data[base:base + ch.audio_len])

    tramp = ad._SinkTrampoline(slicing_sink, copy=False)
    c, _keep = _fake_chunk(nsec=2, sector_len=2352)
    assert tramp.cffi_callback(ffi.NULL, c) == 0
    assert tramp.error is None, f"read_to_file's own pattern tripped: {tramp.error!r}"
    assert out.tell() == 2 * 2352


def test_sink_zero_copy_clean_use_is_not_flagged():
    """The guard must not fire on correct code — otherwise it is unusable."""
    total = []
    tramp = ad._SinkTrampoline(lambda ch: total.append(bytes(ch.data)), copy=False)
    c, _keep = _fake_chunk()
    assert tramp.cffi_callback(ffi.NULL, c) == 0
    assert tramp.error is None
    assert len(total[0]) == 2 * 2352


# ---------------------------------------------------------------------------
# status map decoding
# ---------------------------------------------------------------------------


def test_map_state_and_severity_split():
    assert ad.map_state(0x01) is ad.MapState.OK
    assert ad.map_state(0x43) is ad.MapState.HARD
    assert ad.map_severity(0x43) == 4
    assert ad.map_state(0x00) is ad.MapState.PENDING
    for st in ad.MapState:
        assert ad.map_state(int(st)) is st


def test_map_states_match_the_c_constants():
    assert int(ad.MapState.PENDING) == lib.ACCUDISC_MAP_PENDING
    assert int(ad.MapState.OK) == lib.ACCUDISC_MAP_OK
    assert int(ad.MapState.C2) == lib.ACCUDISC_MAP_C2
    assert int(ad.MapState.HARD) == lib.ACCUDISC_MAP_HARD
    assert int(ad.MapState.RECOVERED) == lib.ACCUDISC_MAP_RECOVERED
    assert int(ad.MapState.SUSPECT) == lib.ACCUDISC_MAP_SUSPECT


# ---------------------------------------------------------------------------
# MSF
# ---------------------------------------------------------------------------


def test_msf_lba_roundtrip():
    for lba in (0, 1, 74, 75, 150, 4499, 100000, 359849):
        assert ad.msf_to_lba(*ad.lba_to_msf(lba)) == lba


def test_lba_zero_is_two_seconds():
    assert ad.lba_to_msf(0) == (0, 2, 0)


def test_deep_leadin_clamps_rather_than_wrapping():
    """Below -150 is before 00:00:00, which MSF cannot represent."""
    assert ad.lba_to_msf(-200) == (0, 0, 0)


# ---------------------------------------------------------------------------
# TOC model and the pure guards (synthetic TOC — no disc needed)
# ---------------------------------------------------------------------------

def _enhanced_cd() -> ad.Toc:
    """Session 1: audio tracks 1-2. Session 2: data track 3, across a seam.

    Geometry modelled on the PX-716A measurement in the header: session 1's
    lead-out sits well before session 2's first track, and the sectors between
    hold no track payload.
    """
    tracks = (
        ad.Track(number=1, adr_ctrl=0x10, session=1, lba=0, sectors=10000, pregap=0),
        ad.Track(number=2, adr_ctrl=0x10, session=1, lba=10000, sectors=10000, pregap=0),
        ad.Track(number=3, adr_ctrl=0x14, session=2, lba=31400, sectors=5000, pregap=0),
    )
    sessions = (
        ad.Session(1, 1, 2, 2, 0, 20000),
        ad.Session(2, 3, 3, 0, 1, 36400),
    )
    return ad.Toc(1, 3, tracks, sessions, 36400, ad.Anomaly(0), 2)


def test_toc_roundtrip_is_byte_faithful():
    """Every pure guard answers about a RECONSTRUCTED struct, not the library's.

    ``_toc_to_c`` rebuilds an ``accudisc_toc`` from the Python dataclass so the
    pure guards can be called. If it dropped a field, the guard would return a
    well-formed verdict about a TOC that is not the disc's — this project's
    named failure class, sitting inside a safety gate.

    ``ffi.new`` zero-fills, including padding, so a bytewise compare of the
    original against the round-trip is exact.
    """
    src = ffi.new("accudisc_toc*")
    src.first_track = 1
    src.last_track = 3
    src.track_count = 3
    src.leadout_lba = 36400
    src.anomalies = int(ad.Anomaly.PAST_LEADOUT | ad.Anomaly.EMPTY_TRACK)
    src.sessions_total = 2
    src.session_count = 2
    for i, (num, ctrl, sess, lba, sec, pre) in enumerate((
            (1, 0x10, 1, 0, 10000, 0),
            (2, 0x10, 1, 10000, 10000, 150),
            (3, 0x14, 2, 31400, 5000, 0))):
        src.tracks[i].number = num
        src.tracks[i].adr_ctrl = ctrl
        src.tracks[i].session = sess
        src.tracks[i].lba = lba
        src.tracks[i].sectors = sec
        src.tracks[i].pregap = pre
    for i, (num, ft, lt, at, dt, lo) in enumerate((
            (1, 1, 2, 2, 0, 20000),
            (2, 3, 3, 0, 1, 36400))):
        src.sessions[i].number = num
        src.sessions[i].first_track = ft
        src.sessions[i].last_track = lt
        src.sessions[i].audio_tracks = at
        src.sessions[i].data_tracks = dt
        src.sessions[i].leadout_lba = lo

    back = ad._toc_to_c(ad._toc_from_c(src))
    n = ffi.sizeof("accudisc_toc")
    assert bytes(ffi.buffer(back, n)) == bytes(ffi.buffer(src, n)), (
        "_toc_to_c lost a field on the way through the Python model")


def test_toc_struct_size_is_pinned():
    """accudisc_toc has no `size` field, so growth must be a deliberate act.

    tests/test_abi.c does NOT pin this one (checked 2026-07-27). If a field is
    added in C, the binding would carry it as zero through _toc_to_c and the
    guards would silently answer about the wrong TOC. This makes that growth
    fail here instead.
    """
    assert ffi.sizeof("accudisc_toc") == 2788, (
        "accudisc_toc changed size — re-check every field _toc_to_c copies, "
        "then update this pin")


def test_track_audio_data_classification():
    toc = _enhanced_cd()
    assert [t.number for t in toc.audio_tracks] == [1, 2]
    assert [t.number for t in toc.data_tracks] == [3]
    assert toc.track(3).is_data
    assert not toc.track(3).is_audio


def test_pregap_belongs_to_the_following_track():
    t = ad.Track(number=2, adr_ctrl=0x10, session=1, lba=10000,
                 sectors=10000, pregap=150)
    assert t.full_extent == (9850, 10150)


def test_trusted_is_false_only_for_untrusted_geometry():
    toc = _enhanced_cd()
    assert toc.trusted
    for bit in (ad.Anomaly.LBA_ORDER, ad.Anomaly.OVERLAP, ad.Anomaly.LEADOUT_BEFORE):
        bad = ad.Toc(1, 3, toc.tracks, toc.sessions, toc.leadout_lba, bit, 2)
        assert not bad.trusted, f"{bit!r} must make geometry untrusted"
    # Reported-only anomalies leave the map usable.
    for bit in (ad.Anomaly.PAST_LEADOUT, ad.Anomaly.EMPTY_TRACK,
                ad.Anomaly.NEGATIVE_LBA, ad.Anomaly.BAD_TRACK_NUM,
                ad.Anomaly.RANGE_MISMATCH, ad.Anomaly.BAD_SESSION):
        ok = ad.Toc(1, 3, toc.tracks, toc.sessions, toc.leadout_lba, bit, 2)
        assert ok.trusted, f"{bit!r} is reported-only and must not gate"


def test_anomaly_tokens_are_stable_slugs():
    assert ad.anomaly_token(ad.Anomaly.LBA_ORDER) == "lba_order"
    for bit in ad.Anomaly:
        assert ad.anomaly_token(bit) != "unknown"


def test_session_counts_are_three_distinct_things():
    toc = _enhanced_cd()
    assert toc.mapped_session_count == 2
    assert toc.sessions_total == 2
    degraded = ad.Toc(1, 3, toc.tracks, (), toc.leadout_lba, ad.Anomaly(0), 2)
    # Sessions we cannot MAP, on a disc we know HAS two: the dangerous case.
    assert degraded.mapped_session_count == 0
    assert degraded.sessions_total == 2


def test_default_audio_session_resolves_and_refuses():
    toc = _enhanced_cd()
    assert ad.Device.default_audio_session(toc) == 1
    # Unknown session structure must be INVAL, never a guess of "1".
    flat = ad.Toc(1, 3, toc.tracks, (), toc.leadout_lba, ad.Anomaly(0), 0)
    try:
        ad.Device.default_audio_session(flat)
    except ad.InvalidArgument:
        pass
    else:
        raise AssertionError("unknown session structure must not resolve")


def test_ranges_are_bounded_by_the_owning_session():
    toc = _enhanced_cd()
    assert ad.Device.session_range(toc, 1) == (0, 20000)
    assert ad.Device.track_range(toc, 1, 2) == (0, 20000)


def test_check_audio_range_refuses_across_the_session_seam():
    """The defect this model exists to prevent: 11,400 sectors of nothing."""
    toc = _enhanced_cd()
    good = ad.Device.check_audio_range(toc, 0, 20000)
    assert good.ok and good.reason == "ok"

    across = ad.Device.check_audio_range(toc, 0, 32000)
    assert not across.ok
    assert across.first_bad_lba == 20000, "must stop at session 1's lead-out"

    into_data = ad.Device.check_audio_range(toc, 31400, 100)
    assert not into_data.ok
    assert into_data.reason == "data_track"
    assert into_data.track == 3


# ---------------------------------------------------------------------------
# read() argument handling
# ---------------------------------------------------------------------------


def test_sector_len_matches_the_requested_companions():
    assert ad._sector_len(ad.C2.NONE, ad.Sub.NONE) == 2352
    assert ad._sector_len(ad.C2.PTRS, ad.Sub.NONE) == 2352 + 294
    assert ad._sector_len(ad.C2.PTRS_BEB, ad.Sub.NONE) == 2352 + 296
    assert ad._sector_len(ad.C2.PTRS, ad.Sub.RAW) == 2352 + 294 + 96
    assert ad._sector_len(ad.C2.NONE, ad.Sub.Q) == 2352 + 16


def test_cancel_flag_roundtrips():
    c = ad.Cancel()
    assert not c.cancelled
    c.cancel()
    assert c.cancelled


def _null_device() -> ad.Device:
    """A Device whose handle is the NULL pointer.

    ``_handle`` rejects ``None``, and ``ffi.NULL`` is not ``None``, so a call
    marshals its arguments all the way into ``accudisc_read_cdda(NULL, ...)``
    and comes back ``ERR_INVAL``. That makes the whole argument-marshalling
    path testable with no drive.
    """
    d = ad.Device.__new__(ad.Device)
    d._dev = ffi.NULL
    d._path = "<null>"
    d._log_cb = None
    return d


def test_read_marshals_every_optional_argument():
    """Covers the fields that only a real read would otherwise touch.

    ``req.cancel`` in particular assigns an ``int*`` into a
    ``const volatile int *`` field; if cffi rejected that qualifier mismatch,
    ``cancel=`` would raise TypeError on first use — on the drive.
    """
    dev = _null_device()
    flag = ad.Cancel()
    try:
        dev.read(0, 10, sink=None, c2=ad.C2.PTRS, sub=ad.Sub.RAW,
                 speed_ladder=[32, 16, 8], status_map=True, cancel=flag,
                 retries=3, c2_retries=4, verify_passes=2, overlap_sectors=2,
                 chunk_sectors=16, speed_x=8, any_type=True, allow_unsafe=True)
    except ad.InvalidArgument:
        pass  # reached the library with every field set: the point of the test
    except Exception as exc:  # noqa: BLE001
        raise AssertionError(
            f"marshalling failed before reaching the library: "
            f"{type(exc).__name__}: {exc}") from exc
    else:
        raise AssertionError("a NULL device must not succeed")


def test_read_rejects_a_zero_count():
    dev = _null_device()
    try:
        dev.read(0, 0)
    except ValueError:
        pass
    else:
        raise AssertionError("count must be > 0")


def test_read_span_refuses_over_the_ceiling():
    """A whole-disc read_span is a ~1 GB allocation that looks like a typo."""
    dev = ad.Device.__new__(ad.Device)  # no device needed; the check is first
    try:
        dev.read_span(0, 400000, max_bytes=1 << 20)
    except ValueError as exc:
        assert "ceiling" in str(exc)
    else:
        raise AssertionError("an oversized span must be refused, not allocated")


# ---------------------------------------------------------------------------
# speed ladder
# ---------------------------------------------------------------------------


def _rung(req, rep, measured, mn, mx, equiv, verdict):
    """A C rung row, built in C memory so _rung_from_c sees the real layout."""
    r = ffi.new("accudisc_speed_rung*")
    r.requested_x, r.reported_x, r.measured_cx = req, rep, measured
    r.min_cx, r.max_cx, r.equiv_x, r.verdict = mn, mx, equiv, verdict
    return r[0]


def test_speed_rung_struct_size_is_pinned():
    """An OUT ARRAY with no `size` field: the stride is the whole contract.

    A mismatch between the header this was compiled against and the loaded
    library does not corrupt one field, it corrupts every element past the
    first. There is nothing at runtime to catch that, so it is pinned here.
    Binding the probe is what made this a real version break (API_PLAN §8
    row 6) — growing it now costs a soname bump.
    """
    assert ffi.sizeof("accudisc_speed_rung") == 14, (
        "accudisc_speed_rung changed size — this is no longer a free ABI "
        "break; bump the version and tell cdda2img before updating this pin"
    )


def test_unmeasured_gradient_is_none_not_zero():
    """The named hazard: 0 means "not measured", and 0.00 reads as "stalled"."""
    r = ad._rung_from_c(_rung(8, 8, 801, 0, 0, 0, 0))
    assert r.min_cx is None and r.max_cx is None
    assert r.min_x is None and r.max_x is None
    assert r.spread_cx is None, "an unmeasured gradient must not spread to 0"
    assert r.measured_x == 8.01, "measured_cx is still a real number"


def test_a_measured_gradient_survives():
    """The complement — without it, a wrapper returning None always passes."""
    r = ad._rung_from_c(_rung(40, 40, 2369, 1805, 2822, 0, 1))
    assert (r.min_cx, r.max_cx) == (1805, 2822)
    assert r.min_x == 18.05 and r.max_x == 28.22
    assert r.spread_cx == 1017
    assert r.verdict is ad.Verdict.ADMITTED


def test_points_two_is_refused_not_rounded_down():
    """`points=2` is a request the library cannot honour, not a request for 1.

    Checked without a device: the guard is in the wrapper and must fire before
    anything is opened, so a caller cannot discover it only on hardware.
    """
    d = object.__new__(ad.Device)
    for bad in (2, 4, 5, -1):
        try:
            ad.Device.probe_speed_ladder(d, points=bad)
        except ad.InvalidArgument as exc:
            assert "rather than rounding it down" in str(exc), (
                f"points={bad} was refused, but not for the documented reason: "
                f"{exc}")
        else:
            raise AssertionError(f"points={bad} was accepted")

    # The complement. Without it, a wrapper that refused EVERY value would
    # pass the loop above — and points=3 is the only value that yields a
    # verdict at all, so refusing it would be silent and total.
    for good in (0, 1, 3):
        try:
            ad.Device.probe_speed_ladder(d, points=good)
        except ad.InvalidArgument as exc:
            raise AssertionError(f"points={good} must be accepted: {exc}")
        except ValueError as exc:
            # Reached _handle on a never-opened Device: past the guard, which
            # is the whole assertion. Anything else propagates.
            assert "closed" in str(exc), exc


def test_admitted_ladder_reproduces_the_cli_line():
    """The recorded PX-716A run: 48 duplicates onto 40, 16 quantizes onto 8."""
    rows = [
        _rung(48, 48, 2296, 1664, 2762, 40, 2),
        _rung(40, 40, 2369, 1805, 2822, 0, 1),
        _rung(32, 32, 1944, 1499, 2297, 0, 1),
        _rung(24, 24, 1492, 1176, 1749, 0, 1),
        _rung(16, 8, 801, 800, 801, 8, 3),
        _rung(8, 8, 800, 800, 802, 0, 1),
        _rung(4, 4, 401, 401, 401, 0, 1),
    ]
    rungs = [ad._rung_from_c(r) for r in rows]
    assert ad.Device.admitted_ladder(rungs) == (40, 32, 24, 8, 4)


def test_an_empty_ladder_is_not_the_same_as_no_ladder():
    """points == 1 yields UNKNOWN everywhere, so admitted_ladder() is empty.

    The CLI distinguishes these by printing no line at all. A caller that only
    looks at the tuple cannot, which is why the docstring says to test the
    verdicts — pinned here so the ambiguity stays documented rather than
    becoming a surprise.
    """
    rungs = [ad._rung_from_c(_rung(40, 40, 2369, 0, 0, 0, 0)),
             ad._rung_from_c(_rung(8, 8, 801, 0, 0, 0, 0))]
    assert ad.Device.admitted_ladder(rungs) == ()
    assert all(r.verdict is ad.Verdict.UNKNOWN for r in rungs)


def test_verdict_tokens_match_the_cli_and_unknown_has_none():
    assert ad.Verdict.ADMITTED.token == "admitted"
    assert ad.Verdict.DUPLICATE.token == "duplicate"
    assert ad.Verdict.QUANTIZED.token == "quantized"
    assert ad.Verdict.UNKNOWN.token == "", (
        "UNKNOWN prints no token on the CLI; inventing one here would let a "
        "caller reconstruct a line the CLI never emits"
    )


def test_verdict_values_match_the_c_constants():
    assert ad.Verdict.UNKNOWN == lib.ACCUDISC_RUNG_UNKNOWN
    assert ad.Verdict.ADMITTED == lib.ACCUDISC_RUNG_ADMITTED
    assert ad.Verdict.DUPLICATE == lib.ACCUDISC_RUNG_DUPLICATE
    assert ad.Verdict.QUANTIZED == lib.ACCUDISC_RUNG_QUANTIZED


# ---------------------------------------------------------------------------
# recording (the destructive path — nothing here touches a drive)
# ---------------------------------------------------------------------------


def test_write_opts_size_is_set_and_accepted_by_the_library():
    """The guard added in 0.3.0, exercised through the real entry point.

    ERR_INVAL (not ERR_ABI) from a NULL device is the positive evidence: the
    size negotiation ACCEPTED the struct and execution reached the argument
    checks behind it. A guard that refused everything would fail this.
    """
    o = ffi.new("accudisc_write_opts*")
    o.size = ffi.sizeof("accudisc_write_opts")
    assert lib.accudisc_write(ffi.NULL, b"t.toc", b"a.bin", o, ffi.NULL,
                              ffi.NULL) == lib.ACCUDISC_ERR_INVAL


def test_write_opts_zero_size_is_refused():
    """What a hand-rolled caller who skipped the field produces."""
    o = ffi.new("accudisc_write_opts*")
    o.size = 0
    assert lib.accudisc_write(ffi.NULL, b"t.toc", b"a.bin", o, ffi.NULL,
                              ffi.NULL) == lib.ACCUDISC_ERR_ABI


def test_write_opts_old_layout_size_is_refused():
    """A caller built before 0.3.0 passes 24 bytes starting with `simulate`.

    sizeof did not change when `size` was added, so nothing about the call
    looks wrong. Both possible values of `simulate` must be refused as sizes,
    which is the entire reason that is safe.
    """
    for simulate_value in (0, 1):
        o = ffi.new("accudisc_write_opts*")
        o.size = simulate_value
        assert lib.accudisc_write(ffi.NULL, b"t.toc", b"a.bin", o, ffi.NULL,
                                  ffi.NULL) == lib.ACCUDISC_ERR_ABI


def test_write_result_covers_only_completed_burns():
    """There is no WriteResult for a failure, and that is deliberate.

    The one mistake this API is shaped to prevent is a caller reporting a
    written disc as blank. A failure member would make that a one-line bug.
    """
    assert {r.name for r in ad.WriteResult} == {"OK", "CAVEATS"}
    assert ad.WriteResult.OK.token == "ok"
    assert ad.WriteResult.CAVEATS.token == "caveats"
    assert ad.WriteResult.OK.clean is True
    assert ad.WriteResult.CAVEATS.clean is False, (
        "CAVEATS is not clean — but the disc WAS written"
    )


def test_caveats_is_a_positive_return_not_an_error():
    """`rc > 0` means the burn COMPLETED. _check must not raise on it.

    A caller testing `if rc:` reports a successful burn as a failure; one
    testing `if rc < 0` silently drops the caveat. Both compile. This pins the
    library's own constant to the positive side and the checker to `< 0`.
    """
    assert lib.ACCUDISC_WROTE_WITH_CAVEATS > 0
    assert ad._check(lib.ACCUDISC_WROTE_WITH_CAVEATS) == \
        lib.ACCUDISC_WROTE_WITH_CAVEATS
    assert ad._check(0) == 0
    # The complement: _check must still raise on the negative side, or the
    # assertions above only prove it never raises at all.
    for bad in (lib.ACCUDISC_ERR_INVAL, lib.ACCUDISC_ERR_UNSUPPORTED,
                lib.ACCUDISC_ERR_ABI):
        try:
            ad._check(bad)
        except ad.AccuDiscError:
            pass
        else:
            raise AssertionError(f"_check({bad}) did not raise")


def test_not_blank_maps_to_its_own_exception_type():
    """`result=not_blank` must be distinguishable from `result=error`.

    The CLI separates them because exit 2 covers both and the code alone
    cannot disambiguate. On the binding the type does it.

    Since 0.4.0 the library says it directly: ERR_NOT_BLANK = -13, split out of
    ERR_UNSUPPORTED, which had been exact only BY CENSUS. The assertions below
    pin the split rather than the old census.
    """
    assert ad._ERRORS[lib.ACCUDISC_ERR_NOT_BLANK] is ad.NotBlank
    assert ad._ERRORS[lib.ACCUDISC_ERR_UNSUPPORTED] is ad.Unsupported
    assert issubclass(ad.NotBlank, ad.AccuDiscError)

    # NotBlank is a SIBLING of Unsupported, deliberately not a subclass. A
    # subclass would keep `except Unsupported` catching a not-blank disc, which
    # is the exact ambiguity -13 exists to end — backward compatibility bought
    # by preserving the bug. Old callers break loudly instead, as with the ABI
    # guard.
    assert not issubclass(ad.NotBlank, ad.Unsupported)
    assert not issubclass(ad.Unsupported, ad.NotBlank)

    # The codes are distinct in the header, not just in the map. If these ever
    # collide, every assertion above passes while meaning nothing.
    assert lib.ACCUDISC_ERR_NOT_BLANK != lib.ACCUDISC_ERR_UNSUPPORTED
    assert lib.ACCUDISC_ERR_NOT_BLANK == -13

    assert not issubclass(ad.InvalidArgument, ad.Unsupported)
    assert not issubclass(ad.Unsupported, ad.InvalidArgument)


def test_write_is_reachable_and_marshals_its_options():
    """Every option reaches C, checked by reading the struct back.

    Not run against a device — the point is that `speed` and `byteswap` are not
    silently dropped, which a burn would reveal far too late.
    """
    o = ffi.new("accudisc_write_opts*")
    o.size = ffi.sizeof("accudisc_write_opts")
    o.simulate, o.byteswap, o.speed = 1, 1, 8
    path = ffi.new("char[]", b"/tmp/ct.bin")
    o.cdtext_path = path
    assert o.simulate == 1 and o.byteswap == 1 and o.speed == 8
    assert ffi.string(o.cdtext_path) == b"/tmp/ct.bin"


# ---------------------------------------------------------------------------
# standalone runner (no pytest required)
# ---------------------------------------------------------------------------


def _main() -> int:
    tests = [(n, o) for n, o in sorted(globals().items())
             if n.startswith("test_") and callable(o)]
    failed = 0
    for name, fn in tests:
        try:
            fn()
        except Exception as exc:  # noqa: BLE001 — a test runner reports everything
            failed += 1
            print(f"FAIL {name}: {type(exc).__name__}: {exc}")
        else:
            print(f"ok   {name}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(_main())
