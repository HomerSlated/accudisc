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

import dataclasses
import mmap
import sys
import types

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
    assert ffi.sizeof("accudisc_read_req") == 64
    assert ffi.sizeof("accudisc_read_stats") == 144  # 136 -> 144 in 0.7.0
    assert ffi.sizeof("accudisc_chunk") == 32


def test_error_codes_are_the_headers():
    assert lib.ACCUDISC_ERR_INVAL == -1
    assert lib.ACCUDISC_ERR_NOTFOUND == -10
    assert lib.ACCUDISC_ERR_ABI == -12
    # -11 is RETIRED (was ERR_UNSAFE_COMBINATION, removed in 0.6.0) and must
    # never be reassigned: a consumer built before 0.6.0 maps it to that name.
    # cffi only exposes constants the cdef names, so the direct test is that no
    # error class claims -11 — which is what a reassignment would produce.
    assert -11 not in ad._ERRORS, "-11 was reused; see accudisc.h's retirement note"
    assert not hasattr(lib, "ACCUDISC_ERR_UNSAFE_COMBINATION")
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


def test_subq_states_match_the_c_constants():
    assert int(ad.SubQState.PENDING) == lib.ACCUDISC_SUBQ_PENDING
    assert int(ad.SubQState.OK) == lib.ACCUDISC_SUBQ_OK
    assert int(ad.SubQState.BAD) == lib.ACCUDISC_SUBQ_BAD
    assert int(ad.SubQState.NO_POSITION) == lib.ACCUDISC_SUBQ_NO_POSITION
    assert int(ad.SubQState.NO_AUDIO) == lib.ACCUDISC_SUBQ_NO_AUDIO
    assert int(ad.SubQState.MISPOSITION) == lib.ACCUDISC_SUBQ_MISPOSITION
    for st in ad.SubQState:
        assert ad.subq_state(int(st)) is st


def test_misposition_is_distinct_and_not_a_health_state():
    """MISPOSITION must not collide with, or be mistaken for, OK.

    The frame it describes is CRC-valid and ADR=1 — everything a naive check
    inspects says healthy — so the only thing separating it from OK is the
    comparison against the commanded LBA. If the two ever shared a value, the
    fault this state exists to surface would be invisible again.
    """
    others = {ad.SubQState.PENDING, ad.SubQState.OK, ad.SubQState.BAD,
              ad.SubQState.NO_POSITION, ad.SubQState.NO_AUDIO}
    assert ad.SubQState.MISPOSITION not in others
    assert ad.subq_state(int(ad.SubQState.MISPOSITION)) \
        is ad.SubQState.MISPOSITION


def test_positional_fault_reads_the_misposition_counter():
    import dataclasses
    zero = {f.name: 0 for f in dataclasses.fields(ad.ReadStats)}
    assert ad.ReadStats(**zero).positional_fault is False
    assert ad.ReadStats(**{**zero, "subq_misposition": 17}).positional_fault


def test_subq_and_map_vocabularies_are_not_interchangeable():
    """The numbering collides on purpose; the meanings do not.

    Pinned because the failure is silent: decoding a subq byte with
    ``map_state`` yields a well-formed ``MapState`` naming a state that never
    occurred. If someone ever "unifies" the two enums, this is what should
    object.
    """
    assert int(ad.SubQState.NO_AUDIO) == int(ad.MapState.RECOVERED)
    assert int(ad.SubQState.BAD) == int(ad.MapState.C2)
    # The wrong decoder does not raise — that is the whole problem.
    assert ad.map_state(int(ad.SubQState.NO_AUDIO)) is ad.MapState.RECOVERED


def test_subq_map_requires_raw_subchannel():
    """Refusal, not a uniform lane a renderer would draw as measured data.

    Reaches the library's own check without a drive: the argument validation in
    accudisc_read_cdda runs before the device is touched, so a NULL handle
    still returns ERR_INVAL rather than faulting.
    """
    req = ffi.new("accudisc_read_req*")
    req.size = ffi.sizeof("accudisc_read_req")
    req.count = 10
    buf = ffi.new("uint8_t[]", 10)
    req.subq_map = buf

    for sub in (lib.ACCUDISC_SUB_NONE, lib.ACCUDISC_SUB_Q):
        req.sub = sub
        assert (lib.accudisc_read_cdda(ffi.NULL, req, ffi.NULL, ffi.NULL,
                                       ffi.NULL) == lib.ACCUDISC_ERR_INVAL)

    # And the guard is not simply always-on: with RAW it gets past this check
    # and fails later, on the NULL device. Without this the assertions above
    # would pass against a function that refused everything.
    req.sub = lib.ACCUDISC_SUB_RAW
    rc = lib.accudisc_read_cdda(ffi.NULL, req, ffi.NULL, ffi.NULL, ffi.NULL)
    assert rc == lib.ACCUDISC_ERR_INVAL  # still INVAL, but for the NULL dev
    req.subq_map = ffi.NULL
    assert (lib.accudisc_read_cdda(ffi.NULL, req, ffi.NULL, ffi.NULL,
                                   ffi.NULL) == rc)


def test_public_dataclass_field_names_are_pinned():
    """Consumers destructure these by name; nothing here noticed a rename.

    cdda2img (§161.2) applied our own criterion back to us and it lands: an
    artefact is load-bearing for someone, so does anything on OUR side fail when
    it changes? For `ReadStats`, `Chunk` and `MapState`/`SubQState` the answer
    was no. A rename would break every consumer with this suite green — the
    exact shape of defect we had just closed for the type annotation.

    Names, not count, and no `<=`: an extra field is additive and harmless, a
    RENAMED one is silent breakage, and a set equality on names catches the
    second without forbidding the first from being noticed here deliberately.
    """
    assert {f.name for f in dataclasses.fields(ad.ReadStats)} == {
        "sectors_read", "sectors_flagged", "c2_bits", "hard_errors",
        "max_bits_sector", "first_flagged_lba", "last_flagged_lba",
        "sense_medium", "sense_hardware", "sense_other", "rereads",
        "sectors_recovered", "sectors_suspect", "slips",
        "subq_total", "subq_ok", "subq_misposition",
        "speed_requested_x", "speed_honoured_x",
    }

    assert {f.name for f in dataclasses.fields(ad.Chunk)} == {
        "lba", "nsec", "data", "sector_len", "audio_len", "c2_len", "sub_len",
    }

    # Enum MEMBER names, which consumers write as MapState.HARD and which no
    # value assertion elsewhere would catch being renamed.
    assert {m.name for m in ad.MapState} == {
        "PENDING", "OK", "C2", "HARD", "RECOVERED", "SUSPECT",
    }
    assert {m.name for m in ad.SubQState} == {
        "PENDING", "OK", "BAD", "NO_POSITION", "NO_AUDIO", "MISPOSITION",
    }


def test_features_names_what_the_version_cannot():
    """A binding-only capability signal, because the version cannot carry one.

    `accudisc_version_string()` moves with the C ABI. A change confined to this
    wrapper leaves it untouched, so a consumer pinning on it — cdda2img does —
    cannot detect one, and a version guard written for it is permanently false
    while looking correct.

    Names are added, never removed or repurposed; a name present always means
    the same thing. This asserts the two that shipped, so removing one is a
    deliberate act rather than a refactor.
    """
    assert isinstance(ad.features, frozenset)
    assert "caller_map_buffers" in ad.features
    assert "subq_map" in ad.features
    assert "speed_honoured" in ad.features
    assert "no_such_capability" not in ad.features


def test_speed_quantized_needs_a_nonzero_honoured_speed():
    """Zero honoured means NO ANSWER, and must not read as "not quantized".

    Four distinct states share two fields, and three of them involve a zero:
    nothing requested; requested but the set failed or page 2A did not read
    back; quantized; honoured. The dangerous confusion is the second against
    the fourth — a missing answer silently presenting as "the drive gave you
    what you asked for", which is the wrong direction to be wrong in.

    Built device-free from the dataclass rather than from a read, because the
    property IS the contract and a drive that quantizes is not always present.
    """
    def stats(**kw):
        base = dict.fromkeys(
            (f.name for f in dataclasses.fields(ad.ReadStats)), 0)
        base.update(kw)
        return ad.ReadStats(**base)

    # The signal itself.
    assert stats(speed_requested_x=16, speed_honoured_x=8).speed_quantized

    # No answer. Would be TRUE under a naive `honoured < requested`, which is
    # the bug this asserts against: 0 < 16 without the nonzero guard.
    assert not stats(speed_requested_x=16, speed_honoured_x=0).speed_quantized

    # Honoured exactly, and nothing asked at all.
    assert not stats(speed_requested_x=16, speed_honoured_x=16).speed_quantized
    assert not stats(speed_requested_x=0, speed_honoured_x=0).speed_quantized

    # A drive adopting MORE than asked is not this defect — nothing was lost.
    assert not stats(speed_requested_x=8, speed_honoured_x=16).speed_quantized


def test_read_annotations_stay_introspectable_strings():
    """cdda2img detects `caller_map_buffers` from annotation SOURCE TEXT.

    We told them to feature-detect and, before `features` existed, the only
    signal available was `inspect.signature(Device.read)` — which returns source
    text only while `from __future__ import annotations` is in force. Drop that
    import and every annotation becomes a type object; a string-comparing
    detector then silently reverts to the old code path, on a binding that
    supports the feature. Silent, and in the wrong direction.

    `features` above is the real fix and they should move to it. This pins the
    older contract anyway, because it is live in a consumer today and nothing
    else in this suite would notice it changing.
    """
    import inspect

    params = inspect.signature(ad.Device.read).parameters
    for name in ("status_map", "subq_map"):
        ann = params[name].annotation
        assert isinstance(ann, str), (
            f"{name} annotation is {type(ann).__name__}, not source text — "
            f"`from __future__ import annotations` was probably dropped, which "
            f"silently breaks consumers detecting this feature by signature"
        )
        # Not pinning the exact spelling: widening to `bool | memoryview` later
        # must stay detectable. The property is that it is no longer only bool.
        assert ann != "bool"


def test_lane_buffer_dispatches_on_identity_not_truthiness():
    """`bool` is an `int` subclass and every non-empty buffer is truthy.

    `if value:` cannot tell `True` from a caller's buffer, so it would allocate
    a second one and write the map somewhere the caller never sees — which is
    the exact defect being fixed here, reintroduced one layer down.
    """
    keep = []
    assert ad._lane_buffer("m", False, 4, keep) is None
    assert ad._lane_buffer("m", None, 4, keep) is None

    own = ad._lane_buffer("m", True, 4, keep)
    assert own is not None and len(own) == 4

    buf = bytearray(4)
    cd = ad._lane_buffer("m", buf, 4, keep)
    # The library must write into the CALLER's object, not a copy of it.
    cd[0] = 0xAB
    assert buf[0] == 0xAB, "caller's buffer is not the one the library writes"


def _refuses(exc_type, fragment, fn, *args):
    """Call fn(*args), require exc_type whose message contains `fragment`.

    The fragment is checked because these guards must not merely fail — they
    must say which argument and what to do instead. A ValueError that says
    nothing sends the caller to read the source.
    """
    try:
        fn(*args)
    except exc_type as exc:
        if fragment not in str(exc):
            raise AssertionError(
                f"{exc_type.__name__} raised but message lacks {fragment!r}: "
                f"{exc}"
            ) from None
        return
    raise AssertionError(f"expected {exc_type.__name__} containing {fragment!r}")


def test_lane_buffer_accepts_the_writable_shapes_and_refuses_the_rest():
    keep = []
    for obj in (bytearray(4), mmap.mmap(-1, 4), memoryview(bytearray(8))[2:6]):
        cd = ad._lane_buffer("m", obj, 4, keep)
        assert len(cd) == 4

    # bytes is the plausible mistake: right length, right type family, and
    # useless because the library could not write to it.
    _refuses(TypeError, "writable", ad._lane_buffer, "m", b"\0" * 4, 4, keep)
    _refuses(TypeError, "int", ad._lane_buffer, "m", 17, 4, keep)


def test_lane_buffer_requires_the_exact_length():
    """Not 'at least', and the LONG case is the dangerous one.

    A caller passing one whole-disc buffer for a 1500-sector span would get
    those sectors written at offset 0 — a complete, well-formed map of the
    wrong region, which nothing downstream could detect. The message must name
    the slice, since slicing is the correct answer rather than a smaller buffer.
    """
    keep = []
    _refuses(ValueError, "exactly 4", ad._lane_buffer, "m", bytearray(3), 4, keep)
    _refuses(ValueError, "exactly 4", ad._lane_buffer, "m", bytearray(4000), 4, keep)
    _refuses(ValueError, "memoryview", ad._lane_buffer, "m", bytearray(4000), 4, keep)

    # An EMPTY buffer, found by cdda2img (their §159.5) and missed by both
    # projects' designs. It is the case where the two bugs interlock: an empty
    # buffer is FALSY, so a truthiness dispatch reads a supplied-but-wrong-length
    # buffer as "no map at all" — and the length check that exists to catch it
    # never runs, because dispatch already decided. The guard is disabled by the
    # bug it guards against. Identity dispatch is what keeps this reachable.
    _refuses(ValueError, "exactly 4", ad._lane_buffer, "m", bytearray(0), 4, keep)
    _refuses(ValueError, "exactly 4", ad._lane_buffer, "m",
             memoryview(bytearray(0)), 4, keep)


def test_caller_buffer_is_the_memory_the_request_points_at():
    """The property the whole change exists for, tested without a drive.

    Attach a caller-owned buffer to a real `accudisc_read_req` exactly as
    `Device.read` does, then write through the REQUEST's pointer and observe it
    in the caller's object. If the binding ever copied instead of aliasing, the
    map would fill correctly and the caller would watch a buffer nobody writes
    to — the failure would look like a drive that never starts.

    This does NOT test live observation; that needs a drive and is verified on
    hardware. Named so a pass here cannot be mistaken for it.
    """
    keep = []
    status = bytearray(8)
    subq = bytearray(8)

    req = ffi.new("accudisc_read_req*")
    req.size = ffi.sizeof("accudisc_read_req")
    req.count = 8
    req.sub = lib.ACCUDISC_SUB_RAW
    req.status_map = ad._lane_buffer("status_map", status, 8, keep)
    req.subq_map = ad._lane_buffer("subq_map", subq, 8, keep)

    req.status_map[3] = lib.ACCUDISC_MAP_HARD
    req.subq_map[3] = lib.ACCUDISC_SUBQ_NO_AUDIO
    assert status[3] == lib.ACCUDISC_MAP_HARD
    assert subq[3] == lib.ACCUDISC_SUBQ_NO_AUDIO

    # And the two lanes are distinct memory — one buffer serving both would
    # also satisfy every assertion above.
    assert status[3] != subq[3]
    req.status_map[5] = 0x11
    assert subq[5] == 0, "the two lanes alias the same memory"


def test_subq_map_defaults_off_and_reads_back_none():
    r = ad.ReadResult(lba=0, count=8, stats=None)
    assert r.subq_map is None
    assert r.subq_state_counts() == {}


def test_subq_state_counts_censuses_every_state():
    buf = ffi.new("uint8_t[]", 5)
    for i, st in enumerate((ad.SubQState.OK, ad.SubQState.OK,
                            ad.SubQState.BAD, ad.SubQState.NO_POSITION,
                            ad.SubQState.NO_AUDIO)):
        buf[i] = int(st)
    r = ad.ReadResult(lba=0, count=5, stats=None, _subq=buf)

    counts = r.subq_state_counts()
    assert counts[ad.SubQState.OK] == 2
    assert counts[ad.SubQState.BAD] == 1
    assert counts[ad.SubQState.NO_POSITION] == 1
    assert counts[ad.SubQState.NO_AUDIO] == 1
    assert counts[ad.SubQState.PENDING] == 0
    assert sum(counts.values()) == 5
    assert bytes(r.subq_map) == bytes([1, 1, 2, 3, 4])


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
                 chunk_sectors=16, speed_x=8, any_type=True)
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
# C2/audio alignment
# ---------------------------------------------------------------------------


def _lag(pairs, active, flags, diffs, peak, runner):
    """A C lag struct, built in C memory so the wrapper sees the real layout."""
    c = ffi.new("accudisc_c2_lag*")
    c.lag_pairs, c.sectors_active, c.flags_used = pairs, active, flags
    c.diff_bytes, c.peak_milli, c.runner_milli = diffs, peak, runner
    return c[0]


def test_c2_lag_struct_size_is_pinned():
    """An OUT struct with no `size` field — the layout is the whole contract.

    Only one of these is ever returned, so unlike a rung array a mismatch
    cannot cascade across elements. It does something worse in miniature:
    `lag_pairs` is first, so a header/library skew silently misreads the one
    number the caller acts on, and every remaining field with it.
    """
    assert ffi.sizeof("accudisc_c2_lag") == 20, (
        "accudisc_c2_lag changed size — bump the version and tell cdda2img "
        "before updating this pin"
    )


def test_inconclusive_is_a_sentinel_not_a_number():
    """NOTFOUND fills the struct anyway, so `lag_pairs` is well-formed garbage.

    The named hazard: the library still writes whatever shift scored highest.
    Nothing in the struct says "this did not conclude" — only the return code
    does — so a wrapper reading the struct alone would hand back a confident
    integer for a probe that reached no verdict.
    """
    c = ad._c2_lag_from_c(_lag(3, 12, 400, 900, 210, 190), conclusive=False)
    assert c.lag_pairs is None, "an inconclusive probe must not report a lag"
    assert not c.conclusive
    assert c.sectors_active == 12, "the evidence survives the sentinel"
    assert c.flags_used == 400 and c.diff_bytes == 900


def test_a_conclusive_lag_survives():
    """The complement — without it, a wrapper returning None always passes."""
    c = ad._c2_lag_from_c(_lag(-2, 340, 9001, 21000, 640, 120), conclusive=True)
    assert c.lag_pairs == -2, "a negative lag is a real verdict, not an error"
    assert c.conclusive and c.c2_fired
    assert (c.peak_milli, c.runner_milli) == (640, 120)


def test_a_conclusive_zero_lag_is_not_an_absent_one():
    """`lag_pairs == 0` means MEASURED, and this drive has no lag.

    It is a different fact from `None`, and the distinction is the reason the
    sentinel is None rather than 0. A drive that happens to be aligned would
    otherwise be indistinguishable from a probe that could not conclude — and
    the caller's next move differs: record 0, versus retry on damaged media.
    """
    c = ad._c2_lag_from_c(_lag(0, 500, 8000, 19000, 700, 150), conclusive=True)
    assert c.lag_pairs == 0
    assert c.conclusive, "zero is a verdict"


def test_c2_fired_separates_a_clean_span_from_thin_evidence():
    """Both are inconclusive; they call for different next moves.

    An all-zero struct means no C2 fired at all — the span was clean and there
    was nothing to correlate, so the answer is a damaged span or a higher
    speed. Evidence present but insufficient means the method worked and the
    span was too small. Collapsing both into `lag_pairs is None` loses that,
    which is what cdda2img asked us to surface (§137.10).
    """
    clean = ad._c2_lag_from_c(_lag(0, 0, 0, 0, 0, 0), conclusive=False)
    assert not clean.conclusive and not clean.c2_fired

    thin = ad._c2_lag_from_c(_lag(1, 7, 30, 40, 180, 170), conclusive=False)
    assert not thin.conclusive and thin.c2_fired, (
        "C2 fired but the evidence was thin — distinguishable from a clean span"
    )


def test_probe_c2_lag_refuses_an_lba_past_leadout():
    """The guard is in the wrapper, so it fires before any drive I/O."""
    d = object.__new__(ad.Device)
    d.read_toc = lambda: types.SimpleNamespace(leadout_lba=1000)
    try:
        ad.Device.probe_c2_lag(d, lba=1000)
    except ad.InvalidArgument as exc:
        assert "lead-out" in str(exc), exc
    else:
        raise AssertionError("an lba at lead-out must be refused")

    # The complement: a valid lba must get PAST the guard. Without it, a
    # wrapper that refused every lba would pass the check above.
    try:
        ad.Device.probe_c2_lag(d, lba=0)
    except ad.InvalidArgument as exc:
        raise AssertionError(f"lba=0 must be accepted: {exc}")
    except ValueError as exc:
        assert "closed" in str(exc), exc


# ---------------------------------------------------------------------------
# speed ladder
# ---------------------------------------------------------------------------


def _rung(req, rep, measured, mn, mx, equiv, verdict, bands=(0, 0, 0)):
    """A C rung row, built in C memory so _rung_from_c sees the real layout."""
    r = ffi.new("accudisc_speed_rung*")
    r.requested_x, r.reported_x, r.measured_cx = req, rep, measured
    r.min_cx, r.max_cx, r.equiv_x, r.verdict = mn, mx, equiv, verdict
    r.band_cx = bands
    return r[0]


def test_speed_rung_struct_size_is_pinned():
    """An OUT ARRAY with no `size` field: the stride is the whole contract.

    A mismatch between the header this was compiled against and the loaded
    library does not corrupt one field, it corrupts every element past the
    first. There is nothing at runtime to catch that, so it is pinned here.
    Binding the probe is what made this a real version break (API_PLAN §8
    row 6) — growing it now costs a soname bump. 14 -> 20 at 0.9.0 was the
    first one paid for (band_cx[3], API_PLAN §8 row 13).
    """
    assert ffi.sizeof("accudisc_speed_rung") == 20, (
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


def test_bands_are_locations_not_order_statistics():
    """The reason band_cx exists, stated as a case min/max cannot express.

    A rung whose MIDDLE band is the fastest — a governor step part-way across
    the disc, or one cache-served band. min/max are identical to the healthy
    monotonic case reported below, so anything reading only the summaries sees
    two indistinguishable rungs. The bands separate them.
    """
    bumped = ad._rung_from_c(_rung(40, 40, 2822, 1805, 2822, 0, 1,
                                   bands=(1805, 2822, 2000)))
    healthy = ad._rung_from_c(_rung(40, 40, 2000, 1805, 2822, 0, 1,
                                    bands=(1805, 2000, 2822)))

    assert (bumped.min_cx, bumped.max_cx) == (healthy.min_cx, healthy.max_cx), (
        "the premise of this test: the summaries must be EQUAL, or it is not "
        "testing what the summaries cannot see"
    )
    assert bumped.bands_cx != healthy.bands_cx
    assert bumped.monotonic is False and healthy.monotonic is True
    assert healthy.bands_x == (18.05, 20.0, 28.22)


def test_a_band_stands_alone_but_a_range_does_not():
    """Per-band None, unlike min/max which are withdrawn as a pair.

    An exact rate for one place does not need its neighbours to be valid, so a
    failed outer band must not take the inner and middle down with it.
    """
    r = ad._rung_from_c(_rung(40, 40, 2000, 0, 0, 0, 0, bands=(1805, 2000, 0)))
    assert r.bands_cx == (1805, 2000, None)
    assert r.bands_x == (18.05, 20.0, None)
    assert r.min_cx is None and r.max_cx is None, (
        "an incomplete sweep must still withdraw the RANGE"
    )
    assert r.monotonic is None, "two bands cannot answer a question about three"


def test_one_band_is_not_the_inner_third():
    """points == 1 fills band 0 only — and it is the whole span, not a third."""
    r = ad._rung_from_c(_rung(8, 8, 801, 0, 0, 0, 0, bands=(801, 0, 0)))
    assert r.bands_cx == (801, None, None)
    assert r.monotonic is None, "one band is not a gradient"


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
# disc classification
# ---------------------------------------------------------------------------


def test_disc_probe_sizeof():
    """Pin the OUT struct's layout.

    accudisc_disc_probe has no `size` field (it is an OUT struct, like
    accudisc_c2_lag), so nothing at run time can notice a field appearing or
    changing width. A stale cdef would misread every value after the change
    while still producing well-formed integers.
    """
    assert ffi.sizeof("accudisc_disc_probe") == 10


def test_disc_tokens_are_the_cli_tokens():
    """The tokens are the machine interface, so assert the LITERALS.

    DiscKind.token calls the library, so comparing it against the library
    would be circular — it would pass through any rename. These literals are
    what cli-machine-interface.md publishes and what cdda2img keys decisions
    on, so a C-side rename must fail here.
    """
    assert [k.token for k in ad.DiscKind] == ["NEITHER", "BLANK", "AUDIO"]
    assert [r.token for r in ad.DiscReason] == [
        "audio", "blank", "data_cd", "closed_data",
        "appendable", "no_medium", "not_cd_profile", "unreadable",
    ]
    assert [t.token for t in ad.TrayState] == ["unknown", "closed", "open"]


def test_disc_probe_not_obtained_is_not_a_value():
    """0xff must become None, and 0 must NOT.

    The library uses ACCUDISC_DISC_STATUS_UNKNOWN (0xff) for "the drive did not
    answer". Passed through as an integer it is TRUTHY, so a plain bool() would
    report every drive that declined to answer as holding a CD-RW. The
    anti-assertion naming that wrong answer is the point of this test.

    The other half matters just as much: disc_status == 0 is a real answer
    meaning *empty*, which is the entire basis of the blank verdict, so it must
    survive as 0 rather than being folded into None with the sentinel.
    """
    unknown = lib.ACCUDISC_DISC_STATUS_UNKNOWN

    c = ffi.new("accudisc_disc_probe*")
    c.kind = lib.ACCUDISC_DISC_NEITHER
    c.reason = lib.ACCUDISC_DISC_WHY_NO_MEDIUM
    c.tray = lib.ACCUDISC_TRAY_OPEN
    c.erasable = unknown
    c.disc_status = unknown
    c.profile = 0
    p = ad._disc_probe_from_c(c[0])
    assert p.erasable is None and p.erasable is not True
    assert p.disc_status is None
    assert p.profile is None
    assert p.no_medium and not p.can_rip and not p.can_burn

    c.erasable = 0
    c.disc_status = 0
    c.profile = 0x09
    c.kind = lib.ACCUDISC_DISC_BLANK
    c.reason = lib.ACCUDISC_DISC_WHY_BLANK
    p = ad._disc_probe_from_c(c[0])
    assert p.erasable is False, "erasable 0 became None: a real answer was lost"
    assert p.disc_status == 0, "disc_status 0 became None: 'empty' was lost"
    assert p.profile == 0x09
    assert p.can_burn and not p.can_rip and not p.no_medium


# ---------------------------------------------------------------------------
# CTDB parity repair
# ---------------------------------------------------------------------------
#
# Fixture-free, so it runs everywhere: an all-zero image has all-zero syndromes,
# so all-zero parity is VALID parity for it and damage can be planted by hand.
# The eight-arm A/B against 1.6 GB of real CTDB parity proves the wire format;
# this proves the binding's contract, which is a different question.

_WIRE_STRIDE = 588          # S = 1176 words = one frame
_S = _WIRE_STRIDE * 2
_FRAMES = 6                 # W/S = 6, so sc = 4 rows
_PCM_WORDS = _FRAMES * 1176


def _ctdb_case(npar=2):
    """A clean image, valid parity for it, and an empty erasure bitmap."""
    return (bytearray(_PCM_WORDS * 2),
            bytes(_S * npar * 2),
            bytearray((_PCM_WORDS + 7) // 8))


def _word(row, col=10):
    """PCM word index of one codeword symbol: base + S + col + row*S."""
    return _S + col + row * _S


def _plant(pcm, row, value=0xA53C, col=10):
    w = _word(row, col)
    pcm[w * 2] = value & 0xFF
    pcm[w * 2 + 1] = value >> 8


def _flag(bits, row, col=10):
    w = _word(row, col)
    bits[w >> 3] |= 1 << (w & 7)


def _repair(pcm, parity, erasures=None, npar=2):
    return ad.ctdb_repair(pcm=pcm, parity=parity, npar=npar,
                          wire_stride=_WIRE_STRIDE, image_first_frame=0,
                          image_frames=_FRAMES, erasures=erasures)


def test_ctdb_struct_sizes():
    """Both structs carry a `size` field, and the binding must agree on it."""
    assert ffi.sizeof("accudisc_ctdb_req") == 72
    assert ffi.sizeof("accudisc_ctdb_report") == 40


def test_ctdb_clean_image_is_clean():
    pcm, parity, _ = _ctdb_case()
    r = _repair(pcm, parity)
    assert r.clean and r.verified and r.dirty_columns == 0
    assert r.audio is not None and r.corrections == 0


def test_ctdb_repairs_and_returns_audio():
    pcm, parity, _ = _ctdb_case()
    _plant(pcm, row=1)
    r = _repair(pcm, parity)
    assert r.verified, "one error against npar=2 should be correctable"
    assert r.dirty_columns == 1 and r.corrections == 1
    assert r.audio == bytearray(_PCM_WORDS * 2), "audio was not restored"
    assert r.audio_unverified is None


def test_ctdb_full_capacity_erasures_are_unverified():
    """The contract this class exists for.

    npar erasures consume every check equation, so the errata are determined
    and re-verification is an identity. The repair still happens — `audio` must
    NOT be the thing that is None-tested to detect a refusal — but the strong
    attribute stays empty so a caller writing `if r.audio:` declines it, exactly
    as a C caller writing `rc == ACCUDISC_OK` does.
    """
    pcm, parity, bits = _ctdb_case()
    _plant(pcm, row=1)
    _plant(pcm, row=2, value=0x1234)
    _flag(bits, row=1)
    _flag(bits, row=2)

    r = _repair(pcm, parity, erasures=bits)
    assert r.unverified_columns == 1
    assert r.audio is None, "the weaker result was reachable through .audio"
    assert r.audio_unverified is not None, "nothing was written at all"
    assert not r.verified and not r.refused
    assert r.audio_unverified == bytearray(_PCM_WORDS * 2)


def test_ctdb_refusal_is_a_result_not_an_exception():
    """Two errors exceed npar/2 = 1 with no erasures to help."""
    pcm, parity, _ = _ctdb_case()
    _plant(pcm, row=0)
    _plant(pcm, row=3, value=0x0F0F)
    r = _repair(pcm, parity)
    assert r.refused and r.refused_columns == 1
    assert r.audio is None and r.audio_unverified is None
    assert r.dirty_columns == 1


def test_ctdb_in_place_repair():
    """out may alias pcm; the same object comes back."""
    pcm, parity, _ = _ctdb_case()
    _plant(pcm, row=1)
    r = ad.ctdb_repair(pcm=pcm, parity=parity, npar=2,
                       wire_stride=_WIRE_STRIDE, image_first_frame=0,
                       image_frames=_FRAMES, out=pcm)
    assert r.audio is pcm
    assert pcm == bytearray(_PCM_WORDS * 2)


def test_ctdb_parity_size_mismatch_raises():
    """A blob paired with another entry's npar is otherwise UNDETECTABLE.

    The blob is syndrome-major, so a 16-parity blob's first 8 planes are a
    valid 8-parity code that decodes to plausible corrections. Only the byte
    count can catch it, which is why this is an exception and not a result.
    """
    pcm, parity, _ = _ctdb_case(npar=2)
    try:
        _repair(pcm, parity, npar=4)   # blob is sized for npar=2
    except ad.InvalidArgument:
        return
    raise AssertionError("a blob sized for a different npar was accepted")


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
