# dim_embed_file(<target> <file> <symbol>)
# Generates generated/embedded/<symbol>.h containing `inline constexpr unsigned char <symbol>[]`
# so binary resources (e.g. the default font) ship inside the executable.
function(dim_embed_file target file symbol)
    set(out ${CMAKE_BINARY_DIR}/generated/embedded/${symbol}.h)
    if(NOT EXISTS ${out} OR ${file} IS_NEWER_THAN ${out})
        file(READ ${file} hex HEX)
        string(LENGTH "${hex}" hexLength)
        math(EXPR size "${hexLength} / 2")
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
        file(WRITE ${out} "#pragma once\n// Generated from ${file}\ninline constexpr unsigned long long ${symbol}_size = ${size};\ninline constexpr unsigned char ${symbol}[] = {${bytes}};\n")
    endif()
    target_include_directories(${target} PRIVATE ${CMAKE_BINARY_DIR}/generated)
endfunction()
