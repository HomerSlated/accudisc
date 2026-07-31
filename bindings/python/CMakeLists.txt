# The Python binding, as an installable component of `make install`.
#
# WHY THIS IS NOT JUST `pip install .`
#
# The extension records a RUNPATH so it can find libaccudisc.so.0 without
# LD_LIBRARY_PATH. Until 0.3.0 that RUNPATH was whatever directory the library
# was LINKED from — i.e. <repo>/build/src — which made an installed package
# depend on the build tree still existing at its original path. It "worked"
# only for as long as nobody moved or deleted the checkout, and it failed at
# import time with a message about libaccudisc.so.0, naming nothing that would
# lead you back to the cause.
#
# So the install flavour is built separately, with ACCUDISC_RUNTIME_LIB_DIR
# pointing at the FINAL libdir. It is staged in the build tree rather than
# written over bindings/python/accudisc/, because the development extension
# there must keep pointing at build/src for the test suite. The two files are
# byte-different in exactly one respect that nothing at import time can check —
# which is precisely the kind of difference this project does not leave to
# convention.

# TWO INDEPENDENT DECISIONS, and the recommended arrangement sets them
# OPPOSITE ways — which is why they are two options and not one.
#
#   ACCUDISC_INSTALL_PYTHON  -> copy the package into the PREFIX's site-packages
#   ACCUDISC_INSTALL_WHEEL   -> install the built wheel into DATADIR, for a
#                               consumer's installer (pip/pipx) to pick up
#
# The prefix's site-packages is frequently NOT on any interpreter's sys.path:
# measured on Void, /usr/local/lib/python3.14/site-packages is absent from the
# system interpreter's path, so `make install` to /usr/local left a package that
# imported nowhere. That is not our bug to fix — Python's own
# sysconfig.get_path('purelib', vars={'base': ...}) names the same directory —
# but it does mean the sitedir copy is the WRONG default for a prefix outside
# /usr, and the wheel is the answer there (see install.sh).
#
# Until 2026-07-31 this was one option with an early return(), so
# -DACCUDISC_INSTALL_PYTHON=OFF also deleted the `wheel` target defined below —
# making the recommended combination inexpressible. Measured, not theorised:
# `make wheel` reported "No rule to make target".
option(ACCUDISC_INSTALL_PYTHON
    "install the Python binding into the prefix's site-packages" ON)
option(ACCUDISC_INSTALL_WHEEL
    "install the built wheel into DATADIR/accudisc/wheel (needs `make wheel`)" OFF)

if(NOT ACCUDISC_INSTALL_PYTHON AND NOT ACCUDISC_INSTALL_WHEEL)
    message(STATUS "accudisc: no Python component will be installed "
                   "(-DACCUDISC_INSTALL_PYTHON=OFF -DACCUDISC_INSTALL_WHEEL=OFF)")
    return()
endif()

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_FOUND)
    message(FATAL_ERROR
        "accudisc: ACCUDISC_INSTALL_PYTHON is ON but no Python 3 interpreter "
        "was found. Install one, or configure with "
        "-DACCUDISC_INSTALL_PYTHON=OFF.")
endif()

# cffi is a BUILD requirement, not just a runtime one: API mode compiles C.
# Checked at configure time and fatal, rather than discovered at `make install`
# — which typically runs as root, at the end of a long build, and is the worst
# possible moment to learn a dependency is missing.
execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import cffi; print(cffi.__version__)"
    RESULT_VARIABLE _adsc_cffi_rc
    OUTPUT_VARIABLE _adsc_cffi_ver
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT _adsc_cffi_rc EQUAL 0)
    message(FATAL_ERROR
        "accudisc: ${Python3_EXECUTABLE} cannot import cffi, which the binding "
        "needs to BUILD (API mode compiles against accudisc.h). Install it "
        "(pip install cffi), point at another interpreter with "
        "-DPython3_EXECUTABLE=..., or configure with "
        "-DACCUDISC_INSTALL_PYTHON=OFF.")
endif()
message(STATUS "accudisc: Python binding via ${Python3_EXECUTABLE} "
               "(cffi ${_adsc_cffi_ver})")

# ---- into the prefix's site-packages (ACCUDISC_INSTALL_PYTHON) --------------
#
# Guarded flat rather than indented: the block is ~70 lines and re-indenting it
# would bury a one-line change in a whole-file diff. The closing `endif()` names
# the option it closes.
if(ACCUDISC_INSTALL_PYTHON)

# Where the package lands. Derived from the PREFIX, never from the
# interpreter's own sysconfig: sysconfig reports the path of the interpreter
# that is running, so a --prefix=/tmp/x install would silently write into
# /usr/lib and "succeed". Relative to CMAKE_INSTALL_LIBDIR, so it follows
# lib/lib64 like everything else.
#
# NOTE ON abi3: the extension is version-independent (one file serves 3.10 to
# 3.14), but site-packages is not — this installs into the BUILDING
# interpreter's version directory. Point another interpreter at it with
# PYTHONPATH, or override this, if that is not what you want.
# Through accudisc_derived_cache for the same reason as ACCUDISC_INSTALL_RPATH:
# a plain cache default freezes on the first configure. This one is relative, so
# a prefix change alone cannot stale it — but switching lib/lib64 or building
# against a different interpreter both can, and would install into the previous
# interpreter's version directory while reporting success.
accudisc_derived_cache(ACCUDISC_PYTHON_SITEDIR
    "${CMAKE_INSTALL_LIBDIR}/python${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}/site-packages"
    "where to install the Python package (under the prefix)")

# RUNPATH comes from the shared ACCUDISC_INSTALL_RPATH, not a knob of its own:
# the extension and the CLI load the same libaccudisc.so.0, so a prefix that
# needs a RUNPATH needs it for both. An EMPTY value is honoured as an
# instruction ("record none") rather than read as "unset" — see
# build_accudisc.py, where that distinction is the reason for a membership test.
set(_adsc_py_stage "${CMAKE_CURRENT_BINARY_DIR}/stage")
set(_adsc_py_ext "${_adsc_py_stage}/accudisc/_accudisc.abi3.so")

# Built at BUILD time, not install time: a compile failure belongs in `make`,
# where it is visible and unprivileged, not in a root `make install`.
#
# If cffi ever names the output something other than _accudisc.abi3.so, this
# fails loudly with "output not produced" rather than installing nothing — the
# filename is an assertion here, not an assumption.
add_custom_command(
    OUTPUT "${_adsc_py_ext}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_adsc_py_stage}"
    COMMAND ${CMAKE_COMMAND} -E env
            "ACCUDISC_INCLUDE_DIR=${PROJECT_SOURCE_DIR}/include"
            "ACCUDISC_LIB_DIR=$<TARGET_FILE_DIR:accudisc>"
            "ACCUDISC_RUNTIME_LIB_DIR=${ACCUDISC_INSTALL_RPATH}"
            ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/build_accudisc.py"
            --stage "${_adsc_py_stage}"
    DEPENDS accudisc
            "${CMAKE_CURRENT_SOURCE_DIR}/build_accudisc.py"
            "${PROJECT_SOURCE_DIR}/include/accudisc/accudisc.h"
    COMMENT "Building the installable Python extension (RUNPATH: ${ACCUDISC_INSTALL_RPATH})"
    VERBATIM)

add_custom_target(accudisc_python ALL DEPENDS "${_adsc_py_ext}")

# The pure-Python sources come from the source tree; only the extension comes
# from the staging directory. __pycache__ is excluded because a .pyc compiled
# here would be stale for the installed layout and is regenerated on first
# import anyway.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/accudisc/"
        DESTINATION "${ACCUDISC_PYTHON_SITEDIR}/accudisc"
        FILES_MATCHING PATTERN "*.py"
                       PATTERN "__pycache__" EXCLUDE)
install(FILES "${_adsc_py_ext}"
        DESTINATION "${ACCUDISC_PYTHON_SITEDIR}/accudisc")

# Absolute form of the same destination, derived rather than restated — an
# override of ACCUDISC_PYTHON_SITEDIR must move the strip target with it, or
# the FATAL_ERROR in accudisc_install_strip fires on a path nothing installed.
if(IS_ABSOLUTE "${ACCUDISC_PYTHON_SITEDIR}")
    set(_adsc_py_full "${ACCUDISC_PYTHON_SITEDIR}")
else()
    set(_adsc_py_full "${CMAKE_INSTALL_PREFIX}/${ACCUDISC_PYTHON_SITEDIR}")
endif()
accudisc_install_strip("${_adsc_py_full}/accudisc/_accudisc.abi3.so")

endif() # ACCUDISC_INSTALL_PYTHON

# ---- `wheel` target ---------------------------------------------------------
#
#   cmake --build build --target wheel      -> build/bindings/python/wheel/*.whl
#
# Not part of ALL: it needs `pip` and produces an artefact nobody wants on an
# ordinary build.
#
# The wheel is PREFIX-SPECIFIC and that is not a defect to design away — it is
# the honest consequence of a native extension that must find libaccudisc.so.0.
# Its RUNPATH is ACCUDISC_INSTALL_RPATH, i.e. the prefix THIS build tree was
# configured for, so a wheel and the `make install` that accompanies it belong
# together. Installing this wheel against a different prefix reproduces exactly
# the failure the staging split exists to prevent.
#
# It does NOT require the library to be installed first: it links against the
# build tree and records the install RUNPATH, the same trick the staged
# extension uses. That is discovery branch 1 (explicit env vars), which is why
# no ACCUDISC_REQUIRE_INSTALLED guard is needed here — the build-tree fallback
# that guard exists to refuse is never reached.
#
# Sources are copied to a staging dir first, because setuptools' build_ext
# rebuilds on timestamps and a stale bindings/python/build/ in the source tree
# is silently packaged as-is (see build_accudisc.py's RUNPATH stamp).
set(_adsc_wheel_stage "${CMAKE_CURRENT_BINARY_DIR}/wheelsrc")
set(_adsc_wheel_out "${CMAKE_CURRENT_BINARY_DIR}/wheel")

add_custom_target(wheel
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_adsc_wheel_stage}" "${_adsc_wheel_out}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_adsc_wheel_stage}/accudisc"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/pyproject.toml"
            "${CMAKE_CURRENT_SOURCE_DIR}/setup.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/setup.cfg"
            "${CMAKE_CURRENT_SOURCE_DIR}/build_accudisc.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            "${_adsc_wheel_stage}/"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/accudisc/__init__.py"
            "${_adsc_wheel_stage}/accudisc/"
    COMMAND ${CMAKE_COMMAND} -E env
            "ACCUDISC_INCLUDE_DIR=${PROJECT_SOURCE_DIR}/include"
            "ACCUDISC_LIB_DIR=$<TARGET_FILE_DIR:accudisc>"
            "ACCUDISC_RUNTIME_LIB_DIR=${ACCUDISC_INSTALL_RPATH}"
            ${Python3_EXECUTABLE} -m pip wheel --no-deps --no-build-isolation
            -w "${_adsc_wheel_out}" "${_adsc_wheel_stage}"
    DEPENDS accudisc
    COMMENT "Building abi3 wheel (RUNPATH: ${ACCUDISC_INSTALL_RPATH})"
    VERBATIM)

# ---- installing the wheel (ACCUDISC_INSTALL_WHEEL) --------------------------
#
# WHERE, AND WHY THERE: ${DATADIR}/accudisc/wheel — i.e. UNDER THE PREFIX, and
# that is the whole point rather than an arbitrary choice. The wheel's RUNPATH
# names this build's ACCUDISC_INSTALL_RPATH, so the wheel is only valid for the
# libaccudisc.so.0 installed alongside it. Any fixed global location
# (/usr/share, ~/.local/share) would be wrong by construction the moment a
# second prefix exists: two wheels, same name, different RUNPATHs, and nothing
# at import time can tell them apart.
#
# THE DIRECTORY IS THE CONTRACT, NOT THE FILENAME. A wheel's name carries the
# version and the ABI/platform tags (accudisc-0.4.0-cp310-abi3-linux_x86_64.whl)
# and PEP 427 requires it stay parseable — installers read those tags off the
# name — so we must NOT rename it to something stable, and a consumer must glob:
#
#     ls "$PREFIX"/share/accudisc/wheel/accudisc-*.whl
#
# Deliberately no `accudisc-latest.whl` symlink: it would either be an invalid
# wheel name or a second parseable name for the same file, and an installer
# picking up both is a failure nobody would look for.
#
# NOT part of an ordinary `make install`, hence default OFF: the wheel needs
# pip, and `wheel` is not in ALL, so a default-ON rule would fail every install
# that had not built it.
if(ACCUDISC_INSTALL_WHEEL)
    # Resolved at INSTALL time, not configure time — the file does not exist
    # until `make wheel` runs, and its name is not predictable from here (the
    # platform tag comes from pip, not from us).
    #
    # $ENV{DESTDIR} is prepended by hand because install(CODE) does not get the
    # staging prefix that install(FILES) does — same idiom as
    # accudisc_install_strip in the top-level CMakeLists.
    #
    # Both failure modes are FATAL rather than skipped. "ON but nothing built"
    # would otherwise install nothing and report success, which is precisely the
    # class of silent no-op this project keeps finding.
    install(CODE "
        file(GLOB _adsc_whl \"${_adsc_wheel_out}/*.whl\")
        list(LENGTH _adsc_whl _adsc_whl_n)
        if(_adsc_whl_n EQUAL 0)
            message(FATAL_ERROR
                \"accudisc: ACCUDISC_INSTALL_WHEEL is ON but no wheel exists in \"
                \"${_adsc_wheel_out} — the wheel target is not part of ALL. \"
                \"Build it first:\\n    cmake --build <builddir> --target wheel\")
        elseif(_adsc_whl_n GREATER 1)
            message(FATAL_ERROR
                \"accudisc: \${_adsc_whl_n} wheels in ${_adsc_wheel_out}: \"
                \"\${_adsc_whl}. Installing all of them would leave a consumer's \"
                \"glob ambiguous. Rebuild the target (it clears the directory) \"
                \"or remove the stale ones.\")
        endif()
        file(INSTALL
             DESTINATION \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_DATADIR}/accudisc/wheel\"
             TYPE FILE FILES \${_adsc_whl})
    ")
endif()
