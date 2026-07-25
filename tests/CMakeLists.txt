# Tests link the static lib and may include internal headers from src/ —
# they exercise internals the public ABI deliberately hides.
foreach(t test_version test_cdb test_sense test_toc test_map test_decode
        test_c2lag test_media test_cuesheet test_tocparse test_index_map
        test_rotation test_tocsrc test_disc test_session test_toc_hostile test_rw
        test_resid test_audio_diff test_write_intake test_cdtext_blob
        test_cdtext_encode test_discinfo test_alloc_even test_uncap)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE accudisc_static)
    target_include_directories(${t} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

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
