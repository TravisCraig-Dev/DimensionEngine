# Targets for dependencies that ship without a usable CMake project.

target_link_libraries(volk PUBLIC Vulkan::Headers)
target_link_libraries(volk_headers INTERFACE Vulkan::Headers)

add_library(vma INTERFACE)
target_include_directories(vma SYSTEM INTERFACE ${vma_SOURCE_DIR}/include)
target_link_libraries(vma INTERFACE Vulkan::Headers)

add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})

add_library(cgltf INTERFACE)
target_include_directories(cgltf SYSTEM INTERFACE ${cgltf_SOURCE_DIR})

# bc7enc (BC7 texture compression): one C++ file.
add_library(bc7enc STATIC ${bc7enc_SOURCE_DIR}/bc7enc.cpp)
target_include_directories(bc7enc SYSTEM PUBLIC ${bc7enc_SOURCE_DIR})
set_target_properties(bc7enc PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(bc7enc PRIVATE /W0)
else()
    target_compile_options(bc7enc PRIVATE -w)
endif()
# (its decoder: the tests check what the encoder made)
add_library(bc7decomp STATIC ${bc7enc_SOURCE_DIR}/bc7decomp.cpp)
target_include_directories(bc7decomp SYSTEM PUBLIC ${bc7enc_SOURCE_DIR})
if(MSVC)
    target_compile_options(bc7decomp PRIVATE /W0)
else()
    target_compile_options(bc7decomp PRIVATE -w)
endif()

# ufbx (FBX import): one C file.
add_library(ufbx STATIC ${ufbx_SOURCE_DIR}/ufbx.c)
target_include_directories(ufbx SYSTEM PUBLIC ${ufbx_SOURCE_DIR})
if(MSVC)
    target_compile_options(ufbx PRIVATE /W0)
endif()

# V-HACD (convex decomposition): single header, implemented in ConvexDecomposition.cpp.
add_library(vhacd INTERFACE)
target_include_directories(vhacd SYSTEM INTERFACE ${vhacd_SOURCE_DIR}/include)

# opusfile (Opus in Ogg), without its HTTP streaming.
add_library(opusfile STATIC
    ${opusfile_SOURCE_DIR}/src/opusfile.c ${opusfile_SOURCE_DIR}/src/info.c
    ${opusfile_SOURCE_DIR}/src/internal.c ${opusfile_SOURCE_DIR}/src/stream.c)
# Third-party code's own warnings (GCC/Clang) are not ours to fix: opus and glslang (MSVC already shows none of theirs).
if(NOT MSVC)
    foreach(dep opus glslang SPIRV MachineIndependent GenericCodeGen OSDependent glslang-default-resource-limits glslang-standalone)
        if(TARGET ${dep})
            target_compile_options(${dep} PRIVATE -w)
        endif()
    endforeach()
endif()
target_include_directories(opusfile SYSTEM PUBLIC ${opusfile_SOURCE_DIR}/include PRIVATE ${opusfile_SOURCE_DIR}/src)
target_link_libraries(opusfile PUBLIC Ogg::ogg Opus::opus)
if(MSVC)
    target_compile_options(opusfile PRIVATE /W0)
    target_compile_definitions(opusfile PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# msdfgen's core: shapes, edge coloring and distance field generation (no FreeType; outlines come from stb_truetype).
file(GLOB DIM_MSDFGEN_SOURCES ${msdfgen_SOURCE_DIR}/core/*.cpp)
add_library(msdfgen STATIC ${DIM_MSDFGEN_SOURCES})
# The config header its own CMake project would generate (a static library: no export macros).
set(DIM_MSDFGEN_CONFIG_DIR ${CMAKE_BINARY_DIR}/generated/msdfgen)
file(WRITE ${DIM_MSDFGEN_CONFIG_DIR}/msdfgen/msdfgen-config.h
     "#pragma once
#define MSDFGEN_PUBLIC
#define MSDFGEN_EXT_PUBLIC
#define MSDFGEN_VERSION 1.12
"
     "#define MSDFGEN_VERSION_MAJOR 1
#define MSDFGEN_VERSION_MINOR 12
#define MSDFGEN_VERSION_REVISION 0
#define MSDFGEN_COPYRIGHT_YEAR 2024
")
target_include_directories(msdfgen SYSTEM PUBLIC ${msdfgen_SOURCE_DIR} ${DIM_MSDFGEN_CONFIG_DIR})
if(MSVC)
    target_compile_options(msdfgen PRIVATE /W0)
endif()

# VST3 hosting: loading plug-in modules, their factories, process data, events and parameter changes.
set(DIM_VST3 ${vst3sdk_SOURCE_DIR})
set(DIM_VST3_SOURCES
    ${DIM_VST3}/pluginterfaces/base/conststringtable.cpp ${DIM_VST3}/pluginterfaces/base/coreiids.cpp
    ${DIM_VST3}/pluginterfaces/base/funknown.cpp ${DIM_VST3}/pluginterfaces/base/ustring.cpp
    ${DIM_VST3}/base/source/baseiids.cpp ${DIM_VST3}/base/source/fbuffer.cpp ${DIM_VST3}/base/source/fdebug.cpp
    ${DIM_VST3}/base/source/fdynlib.cpp ${DIM_VST3}/base/source/fobject.cpp ${DIM_VST3}/base/source/fstreamer.cpp
    ${DIM_VST3}/base/source/fstring.cpp ${DIM_VST3}/base/source/timer.cpp ${DIM_VST3}/base/source/updatehandler.cpp
    ${DIM_VST3}/base/thread/source/fcondition.cpp ${DIM_VST3}/base/thread/source/flock.cpp
    ${DIM_VST3}/public.sdk/source/common/commoniids.cpp ${DIM_VST3}/public.sdk/source/common/commonstringconvert.cpp
    ${DIM_VST3}/public.sdk/source/common/memorystream.cpp ${DIM_VST3}/public.sdk/source/common/readfile.cpp
    ${DIM_VST3}/public.sdk/source/vst/hosting/connectionproxy.cpp ${DIM_VST3}/public.sdk/source/vst/hosting/eventlist.cpp
    ${DIM_VST3}/public.sdk/source/vst/hosting/hostclasses.cpp ${DIM_VST3}/public.sdk/source/vst/hosting/module.cpp
    ${DIM_VST3}/public.sdk/source/vst/hosting/parameterchanges.cpp ${DIM_VST3}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${DIM_VST3}/public.sdk/source/vst/hosting/plugprovider.cpp ${DIM_VST3}/public.sdk/source/vst/hosting/processdata.cpp
    ${DIM_VST3}/public.sdk/source/vst/utility/stringconvert.cpp ${DIM_VST3}/public.sdk/source/vst/vstinitiids.cpp)
if(WIN32)
    list(APPEND DIM_VST3_SOURCES ${DIM_VST3}/public.sdk/source/vst/hosting/module_win32.cpp ${DIM_VST3}/public.sdk/source/common/threadchecker_win32.cpp)
elseif(UNIX AND NOT APPLE)
    list(APPEND DIM_VST3_SOURCES ${DIM_VST3}/public.sdk/source/vst/hosting/module_linux.cpp ${DIM_VST3}/public.sdk/source/common/threadchecker_linux.cpp)
endif()
add_library(vst3hosting STATIC ${DIM_VST3_SOURCES})
target_include_directories(vst3hosting SYSTEM PUBLIC ${DIM_VST3})
target_compile_definitions(vst3hosting PRIVATE RELEASE=1)
set_target_properties(vst3hosting PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 17) # (C++20 changes u8string, which it uses)
if(MSVC)
    target_compile_options(vst3hosting PRIVATE /W0)
else()
    target_compile_options(vst3hosting PRIVATE -w)
    target_link_libraries(vst3hosting PUBLIC ${CMAKE_DL_LIBS} pthread)
endif()

add_library(miniaudio INTERFACE)
target_include_directories(miniaudio SYSTEM INTERFACE ${miniaudio_SOURCE_DIR})

# Dear ImGui and ImGuizmo are compiled into the engine DLL so the editor, engine
# and game modules share one ImGui context. This target only carries headers.
set(DIM_IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp)
add_library(imgui_headers INTERFACE)
target_include_directories(imgui_headers SYSTEM INTERFACE
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp ${imguizmo_SOURCE_DIR}/src)
target_compile_definitions(imgui_headers INTERFACE IMGUI_IMPL_VULKAN_USE_VOLK IMGUI_DEFINE_MATH_OPERATORS)
