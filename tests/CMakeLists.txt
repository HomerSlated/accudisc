# Tests link the static lib and may include internal headers from src/ —
# they exercise internals the public ABI deliberately hides.
foreach(t test_version test_cdb test_sense test_toc test_map test_decode
        test_c2lag test_media test_cuesheet test_tocparse test_index_map
        test_rotation test_tocsrc test_disc test_session test_toc_hostile test_rw
        test_resid test_audio_diff)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE accudisc_static)
    target_include_directories(${t} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
