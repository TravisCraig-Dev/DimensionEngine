# dim_compile_shaders(<target> <shader files...>)
# Compiles GLSL to SPIR-V with glslang and emits C headers (uint32_t arrays) that
# are embedded into the binary, so no shader files need to ship next to the exe.
function(dim_compile_shaders target)
    set(out_dir ${CMAKE_BINARY_DIR}/generated/shaders)
    file(MAKE_DIRECTORY ${out_dir})
    set(headers)
    foreach(src IN LISTS ARGN)
        get_filename_component(name ${src} NAME_WE)
        get_filename_component(ext ${src} LAST_EXT)
        string(SUBSTRING ${ext} 1 -1 stage)
        set(var "k${name}_${stage}")
        set(header ${out_dir}/${name}.${stage}.h)
        add_custom_command(
            OUTPUT ${header}
            COMMAND $<TARGET_FILE:glslang-standalone> -V --target-env vulkan1.3 --quiet
                    -I${CMAKE_CURRENT_SOURCE_DIR}/Shaders --vn ${var} -o ${header} ${src}
            DEPENDS ${src} glslang-standalone ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Common.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/MeshCommon.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/MeshSurface.glsl
                    ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/ParticleCommon.glsl
            COMMENT "Compiling shader ${name}.${stage}"
            VERBATIM)
        list(APPEND headers ${header})
    endforeach()
    add_custom_target(${target}_shaders DEPENDS ${headers})
    add_dependencies(${target} ${target}_shaders)
    target_include_directories(${target} PRIVATE ${CMAKE_BINARY_DIR}/generated)
endfunction()
