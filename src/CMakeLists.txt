# libaccudisc — sources listed explicitly per domain module.
# Add new files here as they land; no globbing.

set(ACCUDISC_SOURCES
    accudisc.c
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
    drive/media_db.c
    toc/toc.c
    toc/fulltoc.c
    read/engine.c
    cdda/crc16.c
    cdda/subq.c
    cdda/scan.c
    meta/cdtext.c
    # write/
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
    target_link_libraries(${tgt} PRIVATE ${CMAKE_DL_LIBS})
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
