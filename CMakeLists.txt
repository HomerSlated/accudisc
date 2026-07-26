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

add_compile_options(-Wall -Wextra)

include(GNUInstallDirs)

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
endif()

if(ACCUDISC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# pkg-config file for downstream consumers (including the bindings)
configure_file(cmake/accudisc.pc.in ${CMAKE_BINARY_DIR}/accudisc.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/accudisc.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
