# Tests link the static lib and may include internal headers from src/ —
# they exercise internals the public ABI deliberately hides.
foreach(t test_version test_cdb test_sense test_toc test_map test_decode)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE accudisc_static)
    target_include_directories(${t} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
