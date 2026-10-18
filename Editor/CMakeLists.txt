file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS Source/*.cpp Source/*.h)
add_executable(DimensionEditor ${EDITOR_SOURCES})
target_include_directories(DimensionEditor PRIVATE Source)
target_link_libraries(DimensionEditor PRIVATE DimensionEngine)
target_compile_definitions(DimensionEditor PRIVATE
    DIM_DEFAULT_PROJECT="${CMAKE_SOURCE_DIR}/Projects/Sandbox"
    DIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    DIM_BUILD_DIR="${CMAKE_BINARY_DIR}"
    DIM_CMAKE_COMMAND="${CMAKE_COMMAND}")
# The game module and player are loaded/copied at runtime, not linked.
add_dependencies(DimensionEditor DimensionPlayer SandboxGame)
if(MSVC)
    target_compile_options(DimensionEditor PRIVATE /W4 /wd4100 /wd4201)
endif()
