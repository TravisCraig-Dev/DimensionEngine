cmake_minimum_required(VERSION 3.24)
project(DimensionEngine VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Installer builds (the "dist" preset): no paths into this source tree are compiled in (the editor's Sandbox comes
# from the install), and the tests are left out.
option(DIM_DISTRIBUTION "Build for the installers (cmake/Packaging.cmake)" OFF)

# All executables land in one folder so the editor can find the player for builds.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Linux: the programs find libDimensionEngine.so and the game module next to themselves (packaged builds too).
if(NOT WIN32)
    set(CMAKE_BUILD_RPATH "$ORIGIN")
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
    # Installers ship the built files as they are: only $ORIGIN, none of the build machine's folders (the build
    # tree, the .NET SDK's host pack) that CMake would add for linked libraries.
    if(DIM_DISTRIBUTION)
        set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
    endif()
    # The static third-party libraries end up inside DimensionEngine.so.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if(MSVC)
    add_compile_options(/MP /utf-8 /Zc:preprocessor)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

include(cmake/Dependencies.cmake)
include(cmake/ThirdPartyTargets.cmake)
include(cmake/Shaders.cmake)
include(cmake/EmbedFile.cmake)

add_subdirectory(Engine)
add_subdirectory(Game)
add_subdirectory(Editor)
add_subdirectory(Player)
include(cmake/NetCoreDbg.cmake)

if(NOT DIM_DISTRIBUTION)
    enable_testing()
    add_subdirectory(Tests)
endif()

include(cmake/Packaging.cmake)
