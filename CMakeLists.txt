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

# pkg-config file for downstream consumers (including the bindings)
configure_file(cmake/accudisc.pc.in ${CMAKE_BINARY_DIR}/accudisc.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/accudisc.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
