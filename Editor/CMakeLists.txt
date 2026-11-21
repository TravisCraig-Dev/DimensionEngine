file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS Source/*.cpp Source/*.h)
# A GUI program on Windows: no console window opens with it (it writes to the terminal it was started from, and to
# Editor.log). main() stays the entry point.
add_executable(DimensionEditor WIN32 ${EDITOR_SOURCES} ${CMAKE_SOURCE_DIR}/Engine/Resources/Dimension.rc)
if(MSVC)
    target_link_options(DimensionEditor PRIVATE /ENTRY:mainCRTStartup)
endif()
target_include_directories(DimensionEditor PRIVATE Source)
target_link_libraries(DimensionEditor PRIVATE DimensionEngine)
target_compile_definitions(DimensionEditor PRIVATE DIM_ENGINE_VERSION="${PROJECT_VERSION}")
# Development builds use the source tree: its Sandbox (automated runs' project), its Siege sample (the launch
# screen's), and rebuilding the game module from the editor. Installers (DIM_DISTRIBUTION) leave that out: the sample
# comes from the install's Samples folder.
if(NOT DIM_DISTRIBUTION)
    target_compile_definitions(DimensionEditor PRIVATE
        DIM_DEFAULT_PROJECT="${CMAKE_SOURCE_DIR}/Projects/Sandbox"
        DIM_SAMPLE_PROJECT="${CMAKE_SOURCE_DIR}/Projects/Siege"
        DIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        DIM_BUILD_DIR="${CMAKE_BINARY_DIR}"
        DIM_CMAKE_COMMAND="${CMAKE_COMMAND}")
endif()
# The game module and player are loaded/copied at runtime, not linked.
add_dependencies(DimensionEditor DimensionPlayer DimensionGame SandboxGame)
if(MSVC)
    target_compile_options(DimensionEditor PRIVATE /W4 /wd4100 /wd4201)
endif()
