# Run at build time (not configure time) by the enginex_git_info target, so benchmark CSVs
# and `enginex --version` name the commit that was actually built. A tree with modified
# tracked files is reported as "<sha>-dirty". The header is rewritten only when its content
# changes, so an unchanged revision triggers no recompilation.
set(ENGINEX_GIT_SHA "unknown")
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${SOURCE_DIR}
        OUTPUT_VARIABLE sha
        RESULT_VARIABLE rc
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(rc EQUAL 0 AND sha)
        set(ENGINEX_GIT_SHA "${sha}")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
            WORKING_DIRECTORY ${SOURCE_DIR}
            OUTPUT_VARIABLE status
            RESULT_VARIABLE rc
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT rc EQUAL 0 OR status)
            string(APPEND ENGINEX_GIT_SHA "-dirty")
        endif()
    endif()
endif()
configure_file(${IN} ${OUT} @ONLY)
