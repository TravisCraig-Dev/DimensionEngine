file(GLOB_RECURSE DIM_ENGINE_SOURCES CONFIGURE_DEPENDS Source/*.cpp Source/*.h)
file(GLOB DIM_SHADERS CONFIGURE_DEPENDS Shaders/*.vert Shaders/*.frag Shaders/*.comp)

# The engine is a DLL so that the editor, the player and hot-reloadable game
# modules all share a single copy of engine state (registries, input, assets, ImGui).
add_library(DimensionEngine SHARED ${DIM_ENGINE_SOURCES} ${DIM_IMGUI_SOURCES})
if(MSVC)
    # Third-party: ImGuizmo's signed/unsigned ImGuiID conversions.
    set_source_files_properties(${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp PROPERTIES COMPILE_OPTIONS /wd4245)
endif()
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
    PRIVATE volk glfw stb cgltf ufbx box2d Jolt miniaudio vhacd opusfile msdfgen vst3hosting)
# The VST3 host's sources include the SDK, which wants RELEASE (or DEVELOPMENT) defined; kept off the rest of the engine.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/Source/Dimension/Music/Vst3Host.cpp
                            ${CMAKE_CURRENT_SOURCE_DIR}/Source/Dimension/Music/PluginWindow.cpp PROPERTIES COMPILE_DEFINITIONS RELEASE=1)
target_compile_definitions(DimensionEngine
    PUBLIC GLFW_INCLUDE_NONE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS GLM_ENABLE_EXPERIMENTAL $<$<CONFIG:Debug>:DIM_DEBUG>)
if(WIN32)
    target_compile_definitions(DimensionEngine PRIVATE "IMGUI_API=__declspec(dllexport)" INTERFACE "IMGUI_API=__declspec(dllimport)")
    target_link_libraries(DimensionEngine PRIVATE runtimeobject) # Windows.Gaming.Input (gamepad haptics)
elseif(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED) # plug-in editor windows (Music Mode)
    target_link_libraries(DimensionEngine PRIVATE X11::X11)
    # Video playback through FFmpeg's libraries when their development packages are installed (libavformat-dev,
    # libavcodec-dev, libswscale-dev, libswresample-dev); without them videos do not open on Linux.
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(DIM_FFMPEG IMPORTED_TARGET QUIET libavformat libavcodec libswscale libswresample libavutil)
    endif()
    if(DIM_FFMPEG_FOUND)
        target_link_libraries(DimensionEngine PRIVATE PkgConfig::DIM_FFMPEG)
        target_compile_definitions(DimensionEngine PRIVATE DIM_FFMPEG=1)
        message(STATUS "Video playback: FFmpeg ${DIM_FFMPEG_libavformat_VERSION}")
    else()
        message(STATUS "Video playback: FFmpeg development libraries not found (videos will not open)")
    endif()
endif()
if(MSVC)
    target_compile_options(DimensionEngine PRIVATE /W4 /wd4100 /wd4201 /wd4324 /bigobj) # /bigobj: the component registry has many template instances
endif()
dim_compile_shaders(DimensionEngine ${DIM_SHADERS})

# ---- C# scripting (.NET hosting). Optional: enabled when the .NET SDK with its native host pack is installed.
find_program(DIM_DOTNET_EXE dotnet)
if(DIM_DOTNET_EXE)
    file(REAL_PATH "${DIM_DOTNET_EXE}" _dotnet_exe) # (Linux: /usr/bin/dotnet links into the installation)
    get_filename_component(_dotnet_root "${_dotnet_exe}" DIRECTORY)
    if(WIN32)
        file(GLOB _hostdirs LIST_DIRECTORIES true "${_dotnet_root}/packs/Microsoft.NETCore.App.Host.win-x64/*/runtimes/win-x64/native")
    else()
        # linux-x64 from Microsoft's builds, <distro>-x64 (ubuntu.24.04-x64) from the distribution's.
        file(GLOB _hostdirs LIST_DIRECTORIES true "${_dotnet_root}/packs/Microsoft.NETCore.App.Host.*-x64/*/runtimes/*-x64/native")
    endif()
    list(SORT _hostdirs COMPARE NATURAL)
    list(POP_BACK _hostdirs DIM_NETHOST_DIR)
endif()
if(DIM_NETHOST_DIR AND EXISTS "${DIM_NETHOST_DIR}/nethost.h")
    message(STATUS "C# scripting enabled (nethost: ${DIM_NETHOST_DIR})")
    if(WIN32)
        set(DIM_NETHOST_LINK "${DIM_NETHOST_DIR}/nethost.lib")
        set(DIM_NETHOST_RUNTIME "${DIM_NETHOST_DIR}/nethost.dll")
    else()
        set(DIM_NETHOST_LINK "${DIM_NETHOST_DIR}/libnethost.so")
        set(DIM_NETHOST_RUNTIME "${DIM_NETHOST_DIR}/libnethost.so")
    endif()
    get_filename_component(DIM_NETHOST_FILE "${DIM_NETHOST_RUNTIME}" NAME)
    target_include_directories(DimensionEngine SYSTEM PRIVATE "${DIM_NETHOST_DIR}")
    target_link_libraries(DimensionEngine PRIVATE "${DIM_NETHOST_LINK}")
    target_compile_definitions(DimensionEngine PUBLIC DIM_DOTNET=1)
    add_custom_command(TARGET DimensionEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DIM_NETHOST_RUNTIME}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${DIM_NETHOST_FILE}")
    # Dimension.Core.dll (managed API). dotnet build is incremental, so running it every build is cheap.
    add_custom_target(DimensionManaged ALL
        COMMAND "${DIM_DOTNET_EXE}" build "${CMAKE_SOURCE_DIR}/Engine/Managed/Dimension.Core/Dimension.Core.csproj"
                -c $<IF:$<CONFIG:Debug>,Debug,Release> -o "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/managed" --nologo -v q
        COMMAND "${DIM_DOTNET_EXE}" build "${CMAKE_SOURCE_DIR}/Engine/Managed/Dimension.Analyzers.CodeFixes/Dimension.Analyzers.CodeFixes.csproj"
                -c $<IF:$<CONFIG:Debug>,Debug,Release> -o "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/managed" --nologo -v q
        COMMENT "Building Dimension.Core (C# API) and the scripts' analyzers"
        VERBATIM)
    add_dependencies(DimensionEngine DimensionManaged)
else()
    message(STATUS "C# scripting disabled (.NET SDK native host pack not found)")
endif()
dim_embed_file(DimensionEngine ${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf kDefaultFontData)
if(NOT WIN32)
    set_target_properties(DimensionEngine PROPERTIES PREFIX "") # DimensionEngine.so, as the editor packages it
endif()
