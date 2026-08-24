# assert() IS the assertion mechanism in most of these files, and -DNDEBUG
# compiles every one of them to nothing. CMAKE_BUILD_TYPE=Release supplies
# exactly that (CMAKE_C_FLAGS_RELEASE = "-O3 -DNDEBUG"), and install.sh leaves
# the build directory configured that way — so `ctest` reported a green suite
# that checked NOTHING. Measured 2026-08-08: an assert(0) on the first line of
# test_map's main() exited 0.
#
# This is the project's signature failure aimed at the safety net itself:
# well-formed output, wrong referent, nothing downstream able to tell. A skipped
# test is loud; a test that runs, prints nothing and passes is not.
#
# -UNDEBUG rather than switching build type, because the tests must exercise the
# library AS SHIPPED — optimised, NDEBUG'd internally. Only each test's own
# translation unit gets asserts back. target/directory compile options land
# after CMAKE_C_FLAGS_<CONFIG> on the command line, so the -U wins.
#
# DIRECTORY scope, not per-target, and that is the whole point: the first
# version of this fix was a target_compile_options() inside the loop below, and
# it silently missed test_cli_toc_format, test_sweep and ctdb_ab — three
# add_executable calls that live outside it. The falsification run used a test
# INSIDE the loop, so it could not have caught that. Here a new add_executable
# cannot opt out by being written somewhere else in the file.
#
# test_assert deliberately carries no flag of its own, so it is a genuine guard
# on this line rather than on a copy of it.
add_compile_options(-UNDEBUG)

# Tests link the static lib and may include internal headers from src/ —
# they exercise internals the public ABI deliberately hides.
foreach(t test_version test_cdb test_sense test_toc test_map test_decode
        test_c2lag test_media test_offsets test_cuesheet test_tocparse test_index_map
        test_rotation test_tocsrc test_disc test_session test_toc_hostile test_rw
        test_resid test_audio_diff test_write_intake test_cdtext_blob
        test_cdtext_encode test_discinfo test_alloc_even test_uncap test_plan
        test_abi test_speeds test_rs16 test_rs16_kat test_ctdb_api)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE accudisc_static)
    target_include_directories(${t} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# The guard on the guard. add_compile_options(-UNDEBUG) above is a claim about
# compiler flag ordering, and if it silently stopped holding — a new generator,
# a new CMake, a build type nobody tried, or someone simply deleting the line —
# the whole suite would go quiet again in precisely the way it just did, with no
# output to compare and no test to fail. This one aborts if assert() is inert.
add_executable(test_assert test_assert.c)
add_test(NAME test_assert COMMAND test_assert)

# The version CMake actually built with, so test_version.c can compare the
# header's macros against it instead of against themselves.
target_compile_definitions(test_version PRIVATE
                           ADSC_BUILD_VERSION="${PROJECT_VERSION}")

# The ERR_AMBIGUOUS branch, against a SYNTHETIC table.
#
# Deliberately outside the foreach loop above and NOT linked against
# accudisc_static: it compiles src/drive/offsets.c itself, with ADSC_OFFSETS_DB
# pointed at a fixture, so it exercises the shipped matcher rather than a copy
# of it. Linking the library as well would put a second definition of
# accudisc_offset_for_inquiry in the link, which is why the test stubs
# adsc_dev_identify instead — the one symbol offsets.c needs from elsewhere.
#
# The shipped table has zero conflicting keys, so this branch is unreachable
# from real data and cannot be tested any other way. add_compile_options
# (-UNDEBUG) above is DIRECTORY scope and reaches this target too, which is
# exactly the property that comment promises for add_executable calls written
# outside the loop.
add_executable(test_offsets_ambiguous test_offsets_ambiguous.c
               ${PROJECT_SOURCE_DIR}/src/drive/offsets.c)
target_include_directories(test_offsets_ambiguous PRIVATE
                           ${PROJECT_SOURCE_DIR}/include
                           ${PROJECT_SOURCE_DIR}/src
                           ${CMAKE_CURRENT_SOURCE_DIR})
# The fixture is named offsets_ambiguous_db.inc, not offsets_db.inc: the quoted
# #include form searches src/drive/ first, so a fixture wearing the real name
# would be silently shadowed by the real table.
target_compile_definitions(test_offsets_ambiguous PRIVATE
                           ADSC_OFFSETS_DB="offsets_ambiguous_db.inc")
add_test(NAME test_offsets_ambiguous COMMAND test_offsets_ambiguous)

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

# Safety properties of the `uninstall` target. Does a real install into a temp
# prefix and touches nothing outside it.
#
# It configures and builds a second tree, so it is the slowest test here by far
# — and it is worth that, because the operation it guards is the only
# DESTRUCTIVE one in the build system. A defect in the other 33 tests produces a
# wrong answer; a defect here deletes a directory that was not ours.
#
# Asserts what must NOT happen (a foreign file kept, a directory we never
# populated left alone) as well as what must: no manifest entry survives, and no
# importable `accudisc` remains.
add_test(NAME test_uninstall_safety
         COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/uninstall_safety.sh
                 ${PROJECT_SOURCE_DIR}
                 ${CMAKE_COMMAND}
                 ${Python3_EXECUTABLE})
set_tests_properties(test_uninstall_safety PROPERTIES TIMEOUT 600)

# The installed RUNPATH must name the prefix the tree is CURRENTLY configured
# for. Reconfiguring an existing tree to a new prefix left the old libdir in the
# cache and therefore in the binary — invisible on a prefix the loader already
# searches, fatal on one it does not.
#
# Asserts both directions, because the obvious fix for one breaks the other: the
# derived default follows a prefix change, and a value the user set does not
# move. Uses two temp prefixes so a wrong RUNPATH cannot be masked by ld.so
# finding the library anyway.
add_test(NAME test_install_relocate
         COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/install_relocate.sh
                 ${PROJECT_SOURCE_DIR}
                 ${CMAKE_COMMAND})
set_tests_properties(test_install_relocate PROPERTIES
                     TIMEOUT 900 SKIP_RETURN_CODE 77)

# Phase 3b — A/B parity of src/repair/ against cdda2img's pinned `ctanalyse`
# (tag ctanalyse-ab-baseline, commit 0e94be1). Eight arms over three real disc
# fixtures in /var/tmp; SKIPS (77) when they are absent, which is everywhere but
# this machine — 1.6 GB of disc images are deliberately not in the repo.
#
# The harness is a separate program from theirs and only their JSON OUTPUT is
# read, so no GPL code is linked, included or executed.
#
# Element-wise on the correction list, not on the summary counts, and that
# distinction is load-bearing: an injected position-basis defect returns the
# right NUMBER of corrections at the wrong bytes, and an injected grid window
# reproduces `dirty_columns` exactly (11760 vs 11760) while changing the
# corrections entirely. A count comparison passes both.
# Scalar versus AVX2 sweep, bit-for-bit. SKIPS (77) rather than passing
# vacuously when there is no second kernel to compare against.
add_executable(test_sweep test_sweep.c)
target_link_libraries(test_sweep PRIVATE accudisc_static)
target_include_directories(test_sweep PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_sweep COMMAND test_sweep)
set_tests_properties(test_sweep PROPERTIES SKIP_RETURN_CODE 77)

add_executable(ctdb_ab ctdb_ab.c)
target_link_libraries(ctdb_ab PRIVATE accudisc_static)
target_include_directories(ctdb_ab PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_ctdb_ab
         COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/ctdb_ab.sh $<TARGET_FILE:ctdb_ab>)
set_tests_properties(test_ctdb_ab PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77)
