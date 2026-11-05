file(GLOB_RECURSE DIM_ENGINE_SOURCES CONFIGURE_DEPENDS Source/*.cpp Source/*.h)
file(GLOB DIM_SHADERS CONFIGURE_DEPENDS Shaders/*.vert Shaders/*.frag Shaders/*.comp)

# The engine is a DLL so that the editor, the player and hot-reloadable game
# modules all share a single copy of engine state (registries, input, assets, ImGui).
add_library(DimensionEngine SHARED ${DIM_ENGINE_SOURCES} ${DIM_IMGUI_SOURCES})
# Every symbol is exported (the editor, player and game modules use the engine's classes directly), as
# WINDOWS_EXPORT_ALL_SYMBOLS would, except lambdas' (over 11000, never used across modules): with them the import
# library passes Windows' limit of 65535 symbols (LNK1189).
if(MSVC)
    set(DIM_ENGINE_DEF_DIR "${CMAKE_CURRENT_BINARY_DIR}/def")
    file(GENERATE OUTPUT "${DIM_ENGINE_DEF_DIR}/objects-$<CONFIG>.txt" CONTENT "$<JOIN:$<TARGET_OBJECTS:DimensionEngine>,\n>\n")
    add_custom_command(TARGET DimensionEngine PRE_LINK
        COMMAND "${CMAKE_COMMAND}" -E __create_def "${DIM_ENGINE_DEF_DIR}/all-$<CONFIG>.def" "${DIM_ENGINE_DEF_DIR}/objects-$<CONFIG>.txt"
        COMMAND "${CMAKE_COMMAND}" "-DIN=${DIM_ENGINE_DEF_DIR}/all-$<CONFIG>.def" "-DOUT=${DIM_ENGINE_DEF_DIR}/engine-$<CONFIG>.def"
                -P "${CMAKE_SOURCE_DIR}/cmake/FilterExports.cmake"
        VERBATIM)
    target_link_options(DimensionEngine PRIVATE "/DEF:${DIM_ENGINE_DEF_DIR}/engine-$<CONFIG>.def")
else()
    set_target_properties(DimensionEngine PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
target_include_directories(DimensionEngine PUBLIC Source)

# Public: headers only. Private: libraries whose state must exist exactly once (inside this DLL).
target_link_libraries(DimensionEngine
    PUBLIC volk_headers vma glm::glm nlohmann_json::nlohmann_json imgui_headers
    PRIVATE volk glfw stb cgltf box2d Jolt miniaudio vhacd opusfile msdfgen)
target_compile_definitions(DimensionEngine
    PUBLIC GLFW_INCLUDE_NONE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS GLM_ENABLE_EXPERIMENTAL $<$<CONFIG:Debug>:DIM_DEBUG>
    PRIVATE "IMGUI_API=__declspec(dllexport)"
    INTERFACE "IMGUI_API=__declspec(dllimport)")
if(MSVC)
    target_compile_options(DimensionEngine PRIVATE /W4 /wd4100 /wd4201 /wd4324 /bigobj) # /bigobj: the component registry has many template instances
endif()
dim_compile_shaders(DimensionEngine ${DIM_SHADERS})

# ---- C# scripting (.NET hosting). Optional: enabled when the .NET SDK with its native host pack is installed.
find_program(DIM_DOTNET_EXE dotnet)
if(DIM_DOTNET_EXE)
    get_filename_component(_dotnet_root "${DIM_DOTNET_EXE}" DIRECTORY)
    file(GLOB _hostdirs LIST_DIRECTORIES true "${_dotnet_root}/packs/Microsoft.NETCore.App.Host.win-x64/*/runtimes/win-x64/native")
    list(SORT _hostdirs COMPARE NATURAL)
    list(POP_BACK _hostdirs DIM_NETHOST_DIR)
endif()
if(DIM_NETHOST_DIR AND EXISTS "${DIM_NETHOST_DIR}/nethost.h")
    message(STATUS "C# scripting enabled (nethost: ${DIM_NETHOST_DIR})")
    target_include_directories(DimensionEngine SYSTEM PRIVATE "${DIM_NETHOST_DIR}")
    target_link_libraries(DimensionEngine PRIVATE "${DIM_NETHOST_DIR}/nethost.lib")
    target_compile_definitions(DimensionEngine PUBLIC DIM_DOTNET=1)
    add_custom_command(TARGET DimensionEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DIM_NETHOST_DIR}/nethost.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/nethost.dll")
    # Dimension.Core.dll (managed API). dotnet build is incremental, so running it every build is cheap.
    add_custom_target(DimensionManaged ALL
        COMMAND "${DIM_DOTNET_EXE}" build "${CMAKE_SOURCE_DIR}/Engine/Managed/Dimension.Core/Dimension.Core.csproj"
                -c $<IF:$<CONFIG:Debug>,Debug,Release> -o "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/managed" --nologo -v q
        COMMENT "Building Dimension.Core (C# API)"
        VERBATIM)
    add_dependencies(DimensionEngine DimensionManaged)
else()
    message(STATUS "C# scripting disabled (.NET SDK native host pack not found)")
endif()
dim_embed_file(DimensionEngine ${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf kDefaultFontData)
