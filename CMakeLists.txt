cmake_minimum_required(VERSION 3.16)

# The installed public header is the single source of truth for the version;
# the .so file version and soname are derived from it. Until 2026-07-26 the two
# were independent and had drifted: accudisc.h said 0.1.0 (and the binary
# printed it) while CMake built libaccudisc.so.0.0.1 — two version numbers a
# binding could reasonably key on, disagreeing about what had bumped. An ABI
# policy of "rebuild on a version bump" is unenforceable while that is true.
# tests/test_version.c asserts the two agree, so they cannot drift again.
file(READ "${CMAKE_CURRENT_LIST_DIR}/include/accudisc/accudisc.h" _adsc_hdr)
foreach(_part MAJOR MINOR PATCH)
    if(NOT _adsc_hdr MATCHES "#define ACCUDISC_VERSION_${_part}[ \t]+([0-9]+)")
        message(FATAL_ERROR "accudisc.h: no ACCUDISC_VERSION_${_part} to read")
    endif()
    set(_adsc_${_part} "${CMAKE_MATCH_1}")
endforeach()
unset(_adsc_hdr)

project(accudisc
    VERSION ${_adsc_MAJOR}.${_adsc_MINOR}.${_adsc_PATCH}
    DESCRIPTION "Precision Red Book CD-DA reader/writer"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # SG_IO / ioctl need _GNU_SOURCE-adjacent APIs

option(ACCUDISC_BUILD_CLI "Build the accudisc command-line tool" ON)
option(ACCUDISC_BUILD_TESTS "Build tests" ON)

# Post-link steps. Both default OFF: a build that needs privilege, or that
# throws away symbols, is a bad default for CI, packagers, and anyone building
# this who is not Keith. Enable per build tree:
#
#   cmake -B build -DACCUDISC_STRIP_BINARIES=ON -DACCUDISC_SETCAP_AFTER_BUILD=ON
#
# ON by default (Keith, 2026-07-26) — but see the build-type guard below: this
# is forced OFF for Debug and RelWithDebInfo, so "non-debug targets" is
# structural, not a convention someone has to remember.
option(ACCUDISC_STRIP_BINARIES
    "strip --strip-unneeded the CLI and shared library after each link" ON)
# ON by default (Keith, 2026-07-26): the tool has limited functionality
# without it — every vendor opcode and the whole burn path need CAP_SYS_RAWIO,
# and the capability binds to the inode, so each relink silently disarms the
# binary. Defaulting this ON makes "armed" the normal state instead of a step
# someone has to remember after every build.
#
# This has NO effect on distribution: file capabilities do not survive a
# tarball, a package, or a copy to another machine. It is purely a local
# development convenience.
#
# CONSEQUENCE, stated plainly: on a host with no passwordless rule for
# ACCUDISC_SETCAP_COMMAND the post-link step FAILS, and the build fails with
# it. That is deliberate — see cli/CMakeLists.txt — but it means anyone
# building this who is not us wants:
#
#   cmake -B build -DACCUDISC_SETCAP_AFTER_BUILD=OFF
#
option(ACCUDISC_SETCAP_AFTER_BUILD
    "re-apply cap_sys_rawio to the CLI after each link (needs a passwordless \
privilege rule for setcap; see ACCUDISC_SETCAP_COMMAND)" ON)

# ON by default (Keith, 2026-07-29). Installing is a root operation, so unlike
# the build-time step this needs no doas rule — it calls setcap directly, via
# ACCUDISC_INSTALL_SETCAP_COMMAND below.
#
# This is the DURABLE arming, and the reason TODO task 2 asks for it: a file
# capability binds to the INODE, and the build tree gets a fresh inode on every
# relink, so a capped build-tree binary disarms itself constantly. The installed
# inode is stable, so capping it survives every rebuild.
#
# It is skipped automatically when DESTDIR is set — see cli/CMakeLists.txt.
option(ACCUDISC_SETCAP_ON_INSTALL
    "apply cap_sys_rawio to the INSTALLED CLI (skipped under DESTDIR)" ON)

# Not ACCUDISC_SETCAP_COMMAND: that one carries `doas` because the build runs
# unprivileged. `make install` to a system prefix is already root, and invoking
# doas from a root install is at best redundant and at worst prompts in a
# non-interactive packaging run.
set(ACCUDISC_INSTALL_SETCAP_COMMAND setcap CACHE STRING
    "setcap invocation used at install time (already privileged; no doas)")

# The privileged setcap invocation, as a list. NOTE THE ABSOLUTE PATH: doas
# matches the command string as *written*, not the resolved binary. On this
# host `command -v setcap` reports /bin/setcap, which is the same inode as
# /usr/bin/setcap under usrmerge but does NOT match a doas rule keyed on the
# latter — it falls through to a rule wanting a password and fails with
# "doas: Authentication failed". So do not substitute ${SETCAP_EXECUTABLE}
# here; that variable is precisely the wrong thing to hand to doas.
set(ACCUDISC_SETCAP_COMMAND doas /usr/bin/setcap CACHE STRING
    "privileged setcap invocation (e.g. 'sudo;/usr/sbin/setcap')")

add_compile_options(-Wall -Wextra)

include(GNUInstallDirs)

# RUNPATH recorded in everything installed that loads libaccudisc.so.0 — the
# CLI and the Python extension. ONE knob, because the two must agree: they load
# the same library, so a prefix needing a RUNPATH needs it for both.
#
# MUST STAY AFTER include(GNUInstallDirs). CMAKE_INSTALL_FULL_LIBDIR does not
# exist before that line, so declaring this above it gives the CACHE an empty
# default which then persists across reconfigures. Found exactly that way while
# writing this: the install log read `runtime path ... to ""` and every other
# line looked correct.
#
# The default is not cosmetic. CMake drops the build-tree RPATH at install and
# records NOTHING by default — correct for /usr, silently broken for every
# other prefix. Measured: `cmake --install` to a custom prefix produced a CLI
# that died with "libaccudisc.so.0: cannot open shared object file". Recording
# the installed libdir is right everywhere, and merely redundant where the
# loader already searches that directory.
#
# Set it EMPTY for distro packaging targeting /usr, where a RUNPATH into a
# standard directory is flagged (Debian lintian) and buys nothing.
set(ACCUDISC_INSTALL_RPATH "${CMAKE_INSTALL_FULL_LIBDIR}" CACHE STRING
    "RUNPATH for installed binaries that load libaccudisc (empty = none)")

# Resolve the strip decision BEFORE the subdirectories, because
# add_custom_command(TARGET ...) may only be called in the directory that
# created the target — so src/ and cli/ each attach their own, and both read
# ACCUDISC_STRIP_EFFECTIVE from here.
#
# Debug and RelWithDebInfo are never stripped, whatever the option says: the
# symbols are the entire point of that build tree. Keep a second directory for
# debugging and it is un-strippable by construction —
#
#   cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
#
set(ACCUDISC_STRIP_EFFECTIVE ${ACCUDISC_STRIP_BINARIES})
set(_adsc_nostrip_why "")
if(CMAKE_BUILD_TYPE MATCHES "^([Dd]ebug|RelWithDebInfo)$")
    set(_adsc_nostrip_why "CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
# The sanitizer builds this project actually uses do NOT set CMAKE_BUILD_TYPE
# — they pass flags directly:
#
#   cmake -B build-asan -DCMAKE_C_COMPILER=clang \
#         -DCMAKE_C_FLAGS="-fsanitize=address,undefined"
#
# Stripping those destroys symbolized traces, i.e. the entire value of the
# run, and the build type guard above would never have caught it. Detect the
# flags themselves, not the label someone remembered to set.
elseif(CMAKE_C_FLAGS MATCHES "-fsanitize|(^| )-g([0-9]|gdb)?( |$)")
    set(_adsc_nostrip_why "debug/sanitizer flags in CMAKE_C_FLAGS")
endif()
if(_adsc_nostrip_why)
    if(ACCUDISC_STRIP_BINARIES)
        message(STATUS "accudisc: ${_adsc_nostrip_why} — not stripping")
    endif()
    set(ACCUDISC_STRIP_EFFECTIVE OFF)
endif()
if(ACCUDISC_STRIP_EFFECTIVE)
    find_program(STRIP_EXECUTABLE strip REQUIRED)
endif()

# ---- install-time strip -----------------------------------------------------
#
# Separate from the build-time strip and NOT a duplicate of it: install(TARGETS)
# copies the file, so an unstripped build tree yields an unstripped install
# whatever the post-link step did, and a `cmake --install` into a second prefix
# gets the same treatment as the first.
#
# Deliberately NOT `cmake --install --strip` / CMAKE_INSTALL_DO_STRIP: those
# strip every installed binary including libaccudisc.a, and a static archive's
# symbol table IS its linking interface — --strip-unneeded there breaks every
# static link against it (src/CMakeLists.txt:74-80). Naming the files one at a
# time is what keeps that exemption structural instead of hoping a global switch
# knows about it.
#
# `path` is evaluated at install time and may contain generator expressions.
# DESTDIR is honoured so staged/packaged installs strip the staged copy.
function(accudisc_install_strip path)
    if(NOT ACCUDISC_STRIP_EFFECTIVE)
        return()
    endif()
    install(CODE "
        set(_f \"\$ENV{DESTDIR}${path}\")
        if(NOT EXISTS \"\${_f}\")
            message(FATAL_ERROR
                \"accudisc: nothing to strip at \${_f} — an install rule and \"
                \"this strip rule disagree about where the file lands\")
        endif()
        message(STATUS \"Stripping: \${_f}\")
        execute_process(COMMAND \"${STRIP_EXECUTABLE}\" --strip-unneeded \"\${_f}\"
                        RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR \"accudisc: strip failed on \${_f}\")
        endif()
    ")
endfunction()

add_subdirectory(src)
add_subdirectory(drivers)

if(ACCUDISC_BUILD_CLI)
    add_subdirectory(cli)

    # Vendor SCSI opcodes (driver features: SpeedRead, Q-Check) and MODE
    # SELECT (burning) are blocked by the kernel's unprivileged SG_IO filter;
    # they need CAP_SYS_RAWIO. The base read/rip path does NOT — it uses only
    # filter-permitted MMC commands. So this capability is opt-in, never part
    # of the ordinary build:
    #
    #   doas cmake --build build --target setcap
    #
    # Caveats by design: setcap needs privilege, and a file capability binds
    # to the inode — every rebuild relinks a fresh binary that must be
    # re-capped. For a persistent grant, apply it to the *installed* binary
    # instead. Running the CLI under doas/root is always the simpler path for
    # one-off vendor-feature use.
    find_program(SETCAP_EXECUTABLE setcap)
    if(SETCAP_EXECUTABLE)
        add_custom_target(setcap
            COMMAND ${SETCAP_EXECUTABLE} cap_sys_rawio+ep $<TARGET_FILE:accudisc_cli>
            DEPENDS accudisc_cli
            COMMENT "setcap cap_sys_rawio+ep on the built accudisc (needs privilege; re-run after each rebuild)"
            VERBATIM)
    endif()

    # The automatic post-link strip/setcap steps live in cli/CMakeLists.txt —
    # add_custom_command(TARGET ...) must be issued where the target was
    # created. See there for the ordering constraint.
endif()

if(ACCUDISC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# The Python binding. Last, so its install rules run after libaccudisc's — it
# links against the build tree, but the ordering keeps `make install` readable
# and leaves room for a future variant that links the installed library.
add_subdirectory(bindings/python)

# ---- man pages --------------------------------------------------------------
# Sections follow each page's own .TH line: accudisc.1 is the CLI (User
# Commands), accudisc.8 is the library reference. Section 8 is "system
# administration" by convention and a library API would normally be section 3 —
# left as-is rather than renamed, because the page is self-consistent and
# `man 8 accudisc` is what anyone reading this repo has learned.
install(FILES docs/man/accudisc.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES docs/man/accudisc.8 DESTINATION ${CMAKE_INSTALL_MANDIR}/man8)

# pkg-config file for downstream consumers (including the bindings).
#
# libdir/includedir are resolved through the FULL_ variants rather than pasted
# after ${exec_prefix}: GNUInstallDirs lets CMAKE_INSTALL_LIBDIR be ABSOLUTE
# (-DCMAKE_INSTALL_LIBDIR=/usr/lib64 is a normal thing for a packager to pass),
# and "${exec_prefix}//usr/lib64" is what the naive concatenation produces —
# a .pc file that parses fine and hands every consumer a path that does not
# exist. Kept prefix-relative when it legitimately is, so the .pc stays
# relocatable in the common case.
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(ACCUDISC_PC_LIBDIR "${CMAKE_INSTALL_FULL_LIBDIR}")
else()
    set(ACCUDISC_PC_LIBDIR "\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}")
endif()
if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(ACCUDISC_PC_INCLUDEDIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
else()
    set(ACCUDISC_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
endif()
configure_file(cmake/accudisc.pc.in ${CMAKE_BINARY_DIR}/accudisc.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/accudisc.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
