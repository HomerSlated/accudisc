add_executable(accudisc_cli main.c)
set_target_properties(accudisc_cli PROPERTIES OUTPUT_NAME accudisc)
target_link_libraries(accudisc_cli PRIVATE accudisc)

install(TARGETS accudisc_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
