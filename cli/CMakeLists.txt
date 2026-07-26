add_executable(accudisc_cli main.c format.c)
set_target_properties(accudisc_cli PROPERTIES OUTPUT_NAME accudisc)
target_link_libraries(accudisc_cli PRIVATE accudisc)

install(TARGETS accudisc_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---- automatic post-link steps (opt-in; see the options in ../CMakeLists.txt)
#
# ORDER IS LOAD-BEARING: strip FIRST, setcap SECOND.
#
# Measured 2026-07-26 on this host: `strip` PRESERVES the inode, but the kernel
# clears the `security.capability` xattr on any write to the file — so that a
# capable binary cannot be edited into a privilege escalation. Stripping AFTER
# setcap therefore leaves the binary silently UNARMED, and unarmed fails
# quietly: the vendor path falls back to generic MMC with no error, on our
# binary and on cdda2img's, which is a symlink to the same inode. Inode
# identity is the WRONG property to check here; only `getcap` answers it.
#
# Both commands go in ONE add_custom_command so the ordering is structural
# rather than incidental to declaration order.
set(_adsc_cli_post)
if(ACCUDISC_STRIP_EFFECTIVE)
    list(APPEND _adsc_cli_post
        COMMAND ${STRIP_EXECUTABLE} --strip-unneeded $<TARGET_FILE:accudisc_cli>)
endif()
if(ACCUDISC_SETCAP_AFTER_BUILD)
    list(APPEND _adsc_cli_post
        COMMAND ${ACCUDISC_SETCAP_COMMAND} cap_sys_rawio=ep
                $<TARGET_FILE:accudisc_cli>)
endif()
if(_adsc_cli_post)
    # Deliberately NOT error-suppressed: if the privileged call fails, the
    # build fails. A warn-and-continue would reproduce the exact bug this
    # automates away — a binary everyone believes is armed and is not.
    add_custom_command(TARGET accudisc_cli POST_BUILD ${_adsc_cli_post}
        COMMENT "accudisc post-link: strip then setcap (in that order)"
        VERBATIM)
endif()
