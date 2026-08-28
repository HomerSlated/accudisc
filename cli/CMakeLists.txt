add_executable(accudisc_cli main.c format.c sink.c)
set_target_properties(accudisc_cli PROPERTIES OUTPUT_NAME accudisc)
target_link_libraries(accudisc_cli PRIVATE accudisc)

# Without this the installed CLI has NO RUNPATH — CMake drops the build-tree
# one and records nothing — so it loads libaccudisc.so.0 only if the install
# libdir happens to be on the loader's path. Fine for /usr, fatal for every
# other prefix. See ACCUDISC_INSTALL_RPATH in ../CMakeLists.txt.
set_target_properties(accudisc_cli PROPERTIES
    INSTALL_RPATH "${ACCUDISC_INSTALL_RPATH}")

install(TARGETS accudisc_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---- install-time strip, then setcap ---------------------------------------
#
# SAME ORDERING CONSTRAINT AS THE POST-LINK STEPS BELOW, and for the same
# measured reason: strip preserves the inode but the kernel clears the
# security.capability xattr on any write to the file, so setcap-then-strip
# leaves the binary silently unarmed. Declaration order here IS execution order
# for install rules, so strip is declared first.
#
# Note that install(TARGETS) copies the binary and an xattr does not survive
# that copy — the freshly installed CLI is unarmed regardless of whether the
# build-tree one was capped. So this is not a duplicate of the post-link setcap;
# it is the only thing that arms what actually gets installed.
accudisc_install_strip("${CMAKE_INSTALL_FULL_BINDIR}/accudisc")

if(ACCUDISC_SETCAP_ON_INSTALL)
    install(CODE "
        set(_bin \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_BINDIR}/accudisc\")
        if(NOT \"\$ENV{DESTDIR}\" STREQUAL \"\")
            # Capabilities do not survive tar/cpio/rsync, so capping a staging
            # tree achieves nothing and misleads whoever reads the log. The
            # package's post-install script is where this belongs instead.
            message(STATUS
                \"accudisc: DESTDIR set — NOT applying cap_sys_rawio. \"
                \"Package scripts must run: setcap cap_sys_rawio=ep \"
                \"${CMAKE_INSTALL_FULL_BINDIR}/accudisc\")
        else()
            message(STATUS \"Setting cap_sys_rawio=ep: \${_bin}\")
            execute_process(
                COMMAND ${ACCUDISC_INSTALL_SETCAP_COMMAND} cap_sys_rawio=ep \"\${_bin}\"
                RESULT_VARIABLE _rc)
            if(NOT _rc EQUAL 0)
                # Loud, exactly as at build time: a warn-and-continue here
                # produces a binary everyone believes is armed and is not, and
                # unarmed fails QUIETLY — the vendor path falls back to generic
                # MMC with no error.
                message(FATAL_ERROR
                    \"accudisc: setcap failed on \${_bin} (need root, or a \"
                    \"filesystem carrying capabilities). Re-run the install as \"
                    \"root, or configure with -DACCUDISC_SETCAP_ON_INSTALL=OFF \"
                    \"and accept that vendor opcodes and burning will not work \"
                    \"without running the tool as root.\")
            endif()
        endif()
    ")
endif()

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
