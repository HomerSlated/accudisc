# Removes exactly what `cmake --install` put down, and nothing else.
#
# Driven by install_manifest.txt, which CMake writes on every install. That is
# the only list that is true by construction — a hand-maintained one drifts the
# moment an install rule is added, and drifts SILENTLY, leaving files behind
# that a later install then appears to update.
#
# TWO SAFETY PROPERTIES, both enforced structurally rather than by care:
#
#   1. A DIRECTORY IS ONLY EVER CONSIDERED IF THE INSTALL POPULATED IT — that
#      is, at least one manifest entry lives under it. So a directory this
#      project did not create cannot be targeted even if it is named in
#      CANDIDATES, because naming it is not what authorises removal; having
#      installed into it is.
#
#   2. A DIRECTORY IS ONLY REMOVED WHEN EMPTY. Nothing is deleted recursively.
#      The one nuance is DERIVED artefacts: __pycache__ is made by the
#      interpreter from a file we installed, is absent from the manifest, and
#      would otherwise keep a package directory permanently non-empty. Those
#      are purged BY NAME from a directory the install populated — never a
#      wildcard, never anything we did not cause to exist.
#
# Property 2 is not tidiness. Delete __init__.py and the extension but leave the
# directory, and `site-packages/accudisc/` becomes a PEP 420 namespace package:
# `import accudisc` then SUCCEEDS and yields an empty module with no version, no
# Device and no error. Verified — that is the phantom cdda2img's import guard
# exists to catch, and an uninstall that leaves the import working is worse than
# one that fails loudly.
#
# Invoked by the `uninstall` target; see the root CMakeLists.txt.
#
# Variables (passed with -D by that target):
#   MANIFEST    path to install_manifest.txt
#   CANDIDATES  ;-list of directories that MAY be removed, deepest first
#   DERIVED     ;-list of basenames the interpreter/toolchain creates from
#               installed files, purgeable from a populated directory

if(NOT EXISTS "${MANIFEST}")
    message(FATAL_ERROR
        "accudisc: no install manifest at ${MANIFEST}\n"
        "  Nothing is known to have been installed FROM THIS BUILD TREE, and "
        "guessing is not on offer — an uninstall that deletes paths it merely "
        "expects to exist is a worse failure than not uninstalling.\n"
        "  If you installed from a different build tree, run this from that "
        "one. If the manifest was deleted, remove the files by hand using the "
        "list in the README's Installing section.")
endif()

file(STRINGS "${MANIFEST}" _files)
set(_removed 0)
set(_absent 0)

foreach(_f IN LISTS _files)
    set(_p "$ENV{DESTDIR}${_f}")
    # IS_SYMLINK as well as EXISTS: EXISTS follows the link, so a DANGLING
    # symlink (libaccudisc.so -> a .so.0.4.0 already removed this same loop)
    # reports false and would be left behind.
    if(EXISTS "${_p}" OR IS_SYMLINK "${_p}")
        message(STATUS "Removing: ${_p}")
        file(REMOVE "${_p}")
        math(EXPR _removed "${_removed} + 1")
    else()
        message(STATUS "Already absent: ${_p}")
        math(EXPR _absent "${_absent} + 1")
    endif()
endforeach()

# ---- directories ------------------------------------------------------------
#
# ORDER MATTERS — CANDIDATES must be deepest first. A parent is only empty once
# its children are gone, so `.../accudisc/drivers` precedes `.../accudisc`.
foreach(_d IN LISTS CANDIDATES)
    set(_p "$ENV{DESTDIR}${_d}")
    if(NOT IS_DIRECTORY "${_p}")
        continue()
    endif()

    # PROPERTY 1. Did the install actually put something here? Compared against
    # the manifest, not against expectation. `${_d}/` anchors the match so
    # /usr/local/lib64/accudisc-extra cannot be matched by /usr/local/lib64/accudisc.
    set(_ours FALSE)
    foreach(_f IN LISTS _files)
        if(_f MATCHES "^${_d}/")
            set(_ours TRUE)
            break()
        endif()
    endforeach()
    if(NOT _ours)
        message(STATUS "Not ours (no installed file under it), keeping: ${_p}")
        continue()
    endif()

    # Purge DERIVED artefacts by exact name only. Everything else — a config a
    # user dropped in, another package's files — makes the directory non-empty
    # and therefore keeps it.
    foreach(_name IN LISTS DERIVED)
        if(EXISTS "${_p}/${_name}")
            message(STATUS "Removing derived: ${_p}/${_name}")
            file(REMOVE_RECURSE "${_p}/${_name}")
        endif()
    endforeach()

    # PROPERTY 2. Empty or it stays. REMOVE_RECURSE rather than REMOVE because
    # file(REMOVE) deletes FILES ONLY and no-ops on a directory WITHOUT saying
    # so — the first version of this printed "Removing empty directory" for two
    # directories and left both on disk. The emptiness check above it is what
    # makes the recursive form equivalent to a plain rmdir here.
    file(GLOB _rest LIST_DIRECTORIES TRUE "${_p}/*" "${_p}/.*")
    list(REMOVE_ITEM _rest "${_p}/." "${_p}/..")
    if(_rest)
        message(STATUS "Not empty, keeping: ${_p}")
        foreach(_r IN LISTS _rest)
            message(STATUS "    still there: ${_r}")
        endforeach()
    else()
        message(STATUS "Removing empty directory: ${_p}")
        file(REMOVE_RECURSE "${_p}")
        if(IS_DIRECTORY "${_p}")
            message(WARNING "accudisc: could not remove ${_p}")
        endif()
    endif()
endforeach()

message(STATUS "accudisc: uninstalled ${_removed} file(s), ${_absent} already absent")
if("$ENV{DESTDIR}" STREQUAL "")
    message(STATUS
        "accudisc: if you ran ldconfig after installing, run it again now.")
endif()
