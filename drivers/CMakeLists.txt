# Vendor drivers: external modules, never linked into libaccudisc.
# Each builds to accudisc-drv-<name>.so in ${CMAKE_BINARY_DIR}/drivers
# (point ACCUDISC_DRIVER_DIR there during development).

set(ACCUDISC_DRIVERS_OUTPUT ${CMAKE_BINARY_DIR}/drivers)

function(accudisc_add_driver name)
    add_library(accudisc_drv_${name} MODULE ${ARGN})
    set_target_properties(accudisc_drv_${name} PROPERTIES
        OUTPUT_NAME accudisc-drv-${name}
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY ${ACCUDISC_DRIVERS_OUTPUT}
    )
    # Drivers use only the installed headers, never library internals.
    target_include_directories(accudisc_drv_${name}
        PRIVATE ${PROJECT_SOURCE_DIR}/include)
    install(TARGETS accudisc_drv_${name}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/accudisc/drivers)
endfunction()

accudisc_add_driver(plextor plextor/plextor.c)
