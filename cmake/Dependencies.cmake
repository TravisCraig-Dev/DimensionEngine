# Third-party dependencies, fetched at configure time. No Vulkan SDK is required:
# volk loads vulkan-1.dll from the GPU driver at runtime, and glslang is built
# from source to compile shaders to SPIR-V.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(DIM_VULKAN_SDK_TAG vulkan-sdk-1.4.363.0)

FetchContent_Declare(vulkan_headers GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git GIT_TAG ${DIM_VULKAN_SDK_TAG} GIT_SHALLOW TRUE)
FetchContent_Declare(volk           GIT_REPOSITORY https://github.com/zeux/volk.git                 GIT_TAG ${DIM_VULKAN_SDK_TAG} GIT_SHALLOW TRUE)
FetchContent_Declare(vma            GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE SOURCE_SUBDIR _header_only)
FetchContent_Declare(glfw           GIT_REPOSITORY https://github.com/glfw/glfw.git                 GIT_TAG 3.4    GIT_SHALLOW TRUE)
FetchContent_Declare(glm            GIT_REPOSITORY https://github.com/g-truc/glm.git                GIT_TAG 1.0.3  GIT_SHALLOW TRUE)
FetchContent_Declare(json           GIT_REPOSITORY https://github.com/nlohmann/json.git             GIT_TAG v3.12.0 GIT_SHALLOW TRUE)
FetchContent_Declare(imgui          GIT_REPOSITORY https://github.com/ocornut/imgui.git             GIT_TAG v1.92.9-docking GIT_SHALLOW TRUE)
FetchContent_Declare(imguizmo       GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git  GIT_TAG 105e4fde45e4e74aae8973d98750820ef7aca7e2 SOURCE_SUBDIR _sources_only)
# (ImGuizmo and stb have no releases: pinned to commits, since tracking master breaks reconfigures whenever upstream moves)
FetchContent_Declare(stb            GIT_REPOSITORY https://github.com/nothings/stb.git              GIT_TAG 6b5799bae8a1fe3792691ef4b72ae26434c3e799)
FetchContent_Declare(cgltf          GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git               GIT_TAG v1.15  GIT_SHALLOW TRUE)
FetchContent_Declare(ufbx           GIT_REPOSITORY https://github.com/ufbx/ufbx.git                 GIT_TAG v0.23.1 GIT_SHALLOW TRUE SOURCE_SUBDIR _sources_only)
FetchContent_Declare(box2d          GIT_REPOSITORY https://github.com/erincatto/box2d.git           GIT_TAG v3.1.1 GIT_SHALLOW TRUE)
FetchContent_Declare(jolt           GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git        GIT_TAG v5.6.0 GIT_SHALLOW TRUE SOURCE_SUBDIR Build)
FetchContent_Declare(miniaudio      GIT_REPOSITORY https://github.com/mackron/miniaudio.git         GIT_TAG 0.11.25 GIT_SHALLOW TRUE SOURCE_SUBDIR _header_only)
FetchContent_Declare(vhacd          GIT_REPOSITORY https://github.com/kmammou/v-hacd.git          GIT_TAG v4.1.0 GIT_SHALLOW TRUE SOURCE_SUBDIR _header_only)
# Opus audio: libogg + libopus, read through opusfile (built in ThirdPartyTargets.cmake: it has no CMake project).
FetchContent_Declare(ogg            GIT_REPOSITORY https://github.com/xiph/ogg.git                  GIT_TAG v1.3.6 GIT_SHALLOW TRUE)
FetchContent_Declare(opus           GIT_REPOSITORY https://github.com/xiph/opus.git                 GIT_TAG v1.5.2 GIT_SHALLOW TRUE)
FetchContent_Declare(opusfile       GIT_REPOSITORY https://github.com/xiph/opusfile.git             GIT_TAG v0.12  GIT_SHALLOW TRUE)
# Multi-channel signed distance fields for text (core only, built in ThirdPartyTargets.cmake).
FetchContent_Declare(msdfgen        GIT_REPOSITORY https://github.com/Chlumsky/msdfgen.git          GIT_TAG v1.12  GIT_SHALLOW TRUE SOURCE_SUBDIR _sources_only)
# VST3 plugin hosting (MIT since SDK 3.8): only the hosting sources are built (ThirdPartyTargets.cmake), not its CMake.
FetchContent_Declare(vst3sdk        GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git    GIT_TAG v3.8.1_build_84 GIT_SHALLOW TRUE
                     GIT_SUBMODULES "base;pluginterfaces;public.sdk" SOURCE_SUBDIR _sources_only)
FetchContent_Declare(glslang        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git      GIT_TAG ${DIM_VULKAN_SDK_TAG} GIT_SHALLOW TRUE)

# --- options for sub-projects
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BOX2D_DOCS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)

# Debug builds compile the libraries above optimized (still with debug symbols and the debug runtime): unoptimized
# Jolt, Box2D, glslang and Opus made physics alone take ~45 ms a frame in Demo3D. The engine's own code stays /Od.
# Jolt overrides the flags itself unless OVERRIDE_CXX_FLAGS is off. Turn DIM_OPTIMIZE_DEPENDENCIES off to step into them.
option(DIM_OPTIMIZE_DEPENDENCIES "Compile third-party libraries optimized in Debug builds" ON)
if (MSVC AND DIM_OPTIMIZE_DEPENDENCIES)
    set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
    set(DIM_SAVED_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    set(DIM_SAVED_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    set(CMAKE_C_FLAGS_DEBUG "/Zi /O2 /Ob2")
    set(CMAKE_CXX_FLAGS_DEBUG "/Zi /O2 /Ob2")
endif()

FetchContent_MakeAvailable(vulkan_headers vma volk glfw glm json imgui imguizmo stb cgltf ufbx box2d jolt miniaudio vhacd ogg opus opusfile msdfgen vst3sdk glslang)

if (MSVC AND DIM_OPTIMIZE_DEPENDENCIES)
    set(CMAKE_C_FLAGS_DEBUG "${DIM_SAVED_C_FLAGS_DEBUG}")
    set(CMAKE_CXX_FLAGS_DEBUG "${DIM_SAVED_CXX_FLAGS_DEBUG}")
endif()
