# Strict warnings for EngineX-owned targets only (third-party code is not affected).
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
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
