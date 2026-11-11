# Strict warnings for EngineX-owned targets only (third-party code is not affected).
include(CheckCXXCompilerFlag)

# GCC's flow-sensitive warnings depend on the optimizer (inlining, value ranges) and have
# well-known false positives at -O2/-O3, notably on std::optional copies and sized
# std::vector construction followed by insert, both used in the ITCH and MoldUDP64 code.
# Clang never runs them, so they cannot be checked on the macOS development machine. Under
# GCC they stay enabled and are printed, but they do not fail the build. Every other warning
# is still an error. Each flag is probed first, because older GCC rejects unknown
# -Wno-error= options.
set(ENGINEX_GCC_NONFATAL_WARNINGS
    maybe-uninitialized stringop-overflow array-bounds null-dereference dangling-reference)
set(ENGINEX_GCC_NONFATAL_FLAGS "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    foreach(w IN LISTS ENGINEX_GCC_NONFATAL_WARNINGS)
        string(MAKE_C_IDENTIFIER "ENGINEX_HAVE_W_${w}" var)
        check_cxx_compiler_flag("-W${w}" ${var})
        if(${var})
            list(APPEND ENGINEX_GCC_NONFATAL_FLAGS "-Wno-error=${w}")
        endif()
    endforeach()
endif()

function(enginex_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(ENGINEX_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor
            -Wold-style-cast -Wcast-align -Woverloaded-virtual -Wnull-dereference
            -Wimplicit-fallthrough)
        if(ENGINEX_WERROR)
            target_compile_options(${target} PRIVATE -Werror ${ENGINEX_GCC_NONFATAL_FLAGS})
        endif()
    endif()
endfunction()
