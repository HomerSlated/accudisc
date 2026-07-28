# Tests link the static lib and may include internal headers from src/ —
# they exercise internals the public ABI deliberately hides.
foreach(t test_version test_cdb test_sense test_toc test_map test_decode
        test_c2lag test_media test_cuesheet test_tocparse test_index_map
        test_rotation test_tocsrc test_disc test_session test_toc_hostile test_rw
        test_resid test_audio_diff test_write_intake test_cdtext_blob
        test_cdtext_encode test_discinfo test_alloc_even test_uncap test_plan
        test_abi test_speeds)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE accudisc_static)
    target_include_directories(${t} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# The version CMake actually built with, so test_version.c can compare the
# header's macros against it instead of against themselves.
target_compile_definitions(test_version PRIVATE
                           ADSC_BUILD_VERSION="${PROJECT_VERSION}")

# Golden test for the CLI machine interface. Compiles cli/format.c directly:
# it asserts the bytes the *binary* emits, so it must exercise the binary's own
# formatter, not a copy. Run the executable with --dump to regenerate a golden
# after a deliberate, documented interface change.
add_executable(test_cli_toc_format test_cli_toc_format.c
               ${PROJECT_SOURCE_DIR}/cli/format.c)
target_link_libraries(test_cli_toc_format PRIVATE accudisc_static)
target_include_directories(test_cli_toc_format PRIVATE
                           ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_cli_toc_format COMMAND test_cli_toc_format)

# Device-free surface of the binary itself: usage text, version shape, exit
# codes. Runs the real executable, so it catches drift the C tests cannot see.
add_test(NAME test_cli_surface
         COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/cli_surface.sh
                 $<TARGET_FILE:accudisc_cli>
                 ${CMAKE_CURRENT_SOURCE_DIR}/golden)

# Catches acquisition policy existing in cli/ and src/ at the same time — the
# drift that API_PLAN.md's phase 2 could otherwise introduce silently.
add_test(NAME test_policy_constants
         COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/policy_constants.sh
                 ${PROJECT_SOURCE_DIR})

# Catches an exit-3 path existing in cli/main.c that cli-machine-interface.md
# does not document. That document calls itself a stable interface; on
# 2026-07-26 four subcommands could exit 3 without being listed, and nothing
# could have noticed, because a stale document still compiles.
add_test(NAME test_exit_codes
         COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/exit_codes.sh
                 ${PROJECT_SOURCE_DIR})

# The Python binding, built against THIS tree and exercised device-free. It is
# reachable without hardware because the ABI size rules answer through a NULL
# device, and because a cffi callback can be invoked directly — so the sink
# trampoline's cancel-on-exception and buffer-lifetime behaviour are testable
# with no drive. Skips (77) when python3/cffi are absent, never on a failure.
add_test(NAME test_python_binding
         COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/python_binding.sh
                 ${PROJECT_SOURCE_DIR}
                 $<TARGET_FILE_DIR:accudisc>)
set_tests_properties(test_python_binding PROPERTIES SKIP_RETURN_CODE 77)
