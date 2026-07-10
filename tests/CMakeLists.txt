add_executable(test_version test_version.c)
target_link_libraries(test_version PRIVATE accudisc_static)
add_test(NAME version COMMAND test_version)
