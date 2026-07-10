cmake_minimum_required(VERSION 3.16)

project(accudisc
    VERSION 0.0.1
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
endif()

if(ACCUDISC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# pkg-config file for downstream consumers (including the bindings)
configure_file(cmake/accudisc.pc.in ${CMAKE_BINARY_DIR}/accudisc.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/accudisc.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
