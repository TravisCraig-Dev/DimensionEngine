file(GLOB_RECURSE PLAYER_SOURCES CONFIGURE_DEPENDS Source/*.cpp Source/*.h)
add_executable(DimensionPlayer ${PLAYER_SOURCES})
target_link_libraries(DimensionPlayer PRIVATE DimensionEngine)
target_compile_definitions(DimensionPlayer PRIVATE DIM_DEFAULT_PROJECT="${CMAKE_SOURCE_DIR}/Projects/Sandbox")
add_dependencies(DimensionPlayer SandboxGame)

# Shipping runtime used by Build: same code, no console window; logs go to Player.log next to the exe.
add_executable(DimensionGame WIN32 ${PLAYER_SOURCES})
target_link_libraries(DimensionGame PRIVATE DimensionEngine)
target_compile_definitions(DimensionGame PRIVATE DIM_DEFAULT_PROJECT="${CMAKE_SOURCE_DIR}/Projects/Sandbox" DIM_GAME_BUILD=1)
if(MSVC)
    target_link_options(DimensionGame PRIVATE /ENTRY:mainCRTStartup)
endif()
add_dependencies(DimensionGame SandboxGame)
