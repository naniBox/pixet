# Writes a header carrying the git commit the binary is being built from, and whether the
# working tree was modified at the time.
#
# Run in script mode (cmake -P) from a build-time custom target, deliberately not at
# configure time: a configure-time capture is correct exactly once and then silently goes
# stale, since committing and rebuilding doesn't re-run configure. An About box confidently
# naming the wrong commit is worse than one naming none.
#
# Only rewrites OUTPUT_FILE when the contents actually change. Without that, every single
# build would touch the header and force a rebuild of everything including it.
#
# Usage:
#   cmake -DSRC_DIR=<repo root> -DOUTPUT_FILE=<path> -P cmake/GitVersion.cmake

if(NOT DEFINED SRC_DIR OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "GitVersion.cmake needs -DSRC_DIR= and -DOUTPUT_FILE=")
endif()

find_package(Git QUIET)

# Defaults for a build from a source tarball, an export, or a machine without git - the
# binary still builds and simply reports that it doesn't know.
set(PIXET_GIT_COMMIT "unknown")
set(PIXET_GIT_DIRTY 0)

if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE _hash
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _rc)

    if(_rc EQUAL 0 AND NOT _hash STREQUAL "")
        set(PIXET_GIT_COMMIT "${_hash}")

        # --untracked-files=no is deliberate: an untracked scratch file sitting in the tree
        # doesn't change what got compiled, and counting it would leave every build
        # permanently flagged as modified, which trains you to ignore the flag. A new source
        # file that genuinely IS compiled necessarily also modifies a tracked CMakeLists.txt,
        # so the real case is still caught.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
            WORKING_DIRECTORY "${SRC_DIR}"
            OUTPUT_VARIABLE _status
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT _status STREQUAL "")
            set(PIXET_GIT_DIRTY 1)
        endif()
    endif()
endif()

set(_content
"// Generated at build time by cmake/GitVersion.cmake - do not edit, and do not commit.\n\
#pragma once\n\
#define PIXET_GIT_COMMIT \"${PIXET_GIT_COMMIT}\"\n\
#define PIXET_GIT_DIRTY ${PIXET_GIT_DIRTY}\n")

set(_existing "")
if(EXISTS "${OUTPUT_FILE}")
    file(READ "${OUTPUT_FILE}" _existing)
endif()
if(NOT _existing STREQUAL _content)
    file(WRITE "${OUTPUT_FILE}" "${_content}")
endif()
