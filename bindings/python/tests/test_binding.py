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


def test_version_agrees_with_loaded_library():
    """Header macros vs the .so. CMake single-sources these; a mismatch is skew."""
    assert ad.version == ad.library_version()
    assert ad.version_string() == ".".join(str(n) for n in ad.library_version())


def test_package_version_matches_the_library():
    """pyproject's version is hand-kept; this is what makes that safe.

    A binding whose declared version can drift from the ABI it was built
    against is a pin nobody can rely on — and cdda2img's stated plan is to key
    on exactly this number.
    """
    import tomllib
    from pathlib import Path

    pyproject = Path(__file__).resolve().parent.parent / "pyproject.toml"
    declared = tomllib.loads(pyproject.read_text())["project"]["version"]
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
