# Game code: native C++ scripts and systems for the Sandbox project.
# Built as a DLL that the Editor hot-reloads and the Player loads at startup.
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS Source/*.cpp Source/*.h)
add_library(SandboxGame SHARED ${GAME_SOURCES})
target_include_directories(SandboxGame PRIVATE Source)
target_link_libraries(SandboxGame PRIVATE DimensionEngine)
set_target_properties(SandboxGame PROPERTIES PREFIX "") # SandboxGame.so on Linux, as the project names it
