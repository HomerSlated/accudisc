# libaccudisc — sources listed explicitly per domain module.
# Add new files here as they land; no globbing.

set(ACCUDISC_SOURCES
    accudisc.c
    abi.c
    device.c
    transport/sgio.c
    transport/sense.c
    mmc/cdb.c
    mmc/mmc.c
    drive/features.c
    drive/c2lag.c
    drive/speeds.c
    drive/driver.c
    drive/offsets.c
    drive/uncap.c
    drive/census.c
    drive/media_db.c
    drive/disc.c
    toc/toc.c
    toc/fulltoc.c
    toc/session.c
    toc/plan.c
    read/engine.c
    read/accubuf.c
    write/fifo.c
    cdda/crc16.c
    cdda/subq.c
    cdda/scan.c
    cdda/index_map.c
    cdda/pregap_scan.c
    cdda/rw.c
    repair/gf16.c
    repair/sweep.c
    repair/rs16.c
    repair/ctdb.c
    meta/cdtext.c
    meta/cdtext_blob.c
    meta/cdtext_encode.c
    write/wparams.c
    write/discinfo.c
    write/cuesheet.c
    write/tocparse.c
    write/burn.c
    write/write_api.c
    write/verify.c
    write/write_offset.c
)

add_library(accudisc SHARED ${ACCUDISC_SOURCES})
add_library(accudisc_static STATIC ${ACCUDISC_SOURCES})

foreach(tgt accudisc accudisc_static)
    target_include_directories(${tgt}
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    # AccuBuffer runs the sink on its own thread; see src/read/accubuf.h.
    target_link_libraries(${tgt} PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
    target_compile_definitions(${tgt} PRIVATE
        ACCUDISC_DRIVER_DIR_DEFAULT="${CMAKE_INSTALL_FULL_LIBDIR}/accudisc/drivers")
endforeach()

set_target_properties(accudisc PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    C_VISIBILITY_PRESET hidden
)
set_target_properties(accudisc_static PROPERTIES OUTPUT_NAME accudisc)

install(TARGETS accudisc accudisc_static
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/accudisc
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# The SHARED object only — libaccudisc.a is exempt for the reason given below,
# and the exemption is why this names one file instead of using
# `cmake --install --strip`. TARGET_FILE_NAME resolves to the real versioned
# file (libaccudisc.so.X.Y.Z), not one of the symlinks beside it.
accudisc_install_strip(
    "${CMAKE_INSTALL_FULL_LIBDIR}/$<TARGET_FILE_NAME:accudisc>")

# Opt-in post-link strip (see ACCUDISC_STRIP_BINARIES in ../CMakeLists.txt).
# The SHARED library only — libaccudisc.a is deliberately left alone, because
# a static archive's symbol table IS its linking interface and
# --strip-unneeded there would break every static link against it. Only
# --strip-debug is ever safe on an archive, and we do not need it. The shared
# object keeps .dynsym, so linking and dlopen are unaffected; what goes is
# .symtab, i.e. backtrace quality — hence the Debug/RelWithDebInfo exemption.
if(ACCUDISC_STRIP_EFFECTIVE)
    add_custom_command(TARGET accudisc POST_BUILD
        COMMAND ${STRIP_EXECUTABLE} --strip-unneeded $<TARGET_FILE:accudisc>
        COMMENT "strip --strip-unneeded libaccudisc" VERBATIM)
endif()
