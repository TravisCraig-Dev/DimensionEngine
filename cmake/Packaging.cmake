# Installers: `cmake --preset dist`, `cmake --build --preset dist`, then `cpack` in out/build/dist.
#   Windows: an NSIS setup (Start menu and desktop shortcuts, uninstaller) and a portable .zip.
#   Linux:   a .deb (installs to /opt/dimension, adds a menu entry and `dimension-editor`) and a .tar.gz with install.sh.
# The install tree is the same everywhere: bin/ (programs, the engine library, the game module, the C# assemblies),
# Samples/Siege (the sample project; the editor copies it to the user's Documents to open it) and share/ (icon and
# desktop entry, Linux). The Sandbox stays in the repository as the test bed; it is not shipped.

include(GNUInstallDirs)

# Programs and libraries, all in bin/ (the editor finds the player and game module next to itself). The built files
# themselves (not install(TARGETS), which would want the Windows import libraries too); Linux's are linked with
# RPATH $ORIGIN already.
install(PROGRAMS $<TARGET_FILE:DimensionEditor> $<TARGET_FILE:DimensionPlayer> $<TARGET_FILE:DimensionGame>
        DESTINATION bin COMPONENT Runtime)
install(FILES $<TARGET_FILE:DimensionEngine> DESTINATION bin COMPONENT Runtime)
# C# scripting: the host library and the engine's managed assemblies (scripts compile against bin/managed).
if(WIN32)
    install(FILES "${CMAKE_BINARY_DIR}/bin/nethost.dll" DESTINATION bin OPTIONAL COMPONENT Runtime)
else()
    install(FILES "${CMAKE_BINARY_DIR}/bin/libnethost.so" DESTINATION bin OPTIONAL COMPONENT Runtime)
endif()
install(DIRECTORY "${CMAKE_BINARY_DIR}/bin/managed/" DESTINATION bin/managed OPTIONAL COMPONENT Runtime
        PATTERN "*.pdb" EXCLUDE)
# The .NET runtime itself, in bin/dotnet (CSharpRuntime::Init looks there before the machine's): C# scripts run on a
# machine without .NET. The newest 10.x runtime and host resolver installed here (`dotnet --list-runtimes`).
# (Compiling scripts that change still needs the .NET SDK; the sample's come compiled, below.)
find_program(DIM_DOTNET dotnet)
set(_dotnet_framework "")
if(DIM_DOTNET)
    execute_process(COMMAND "${DIM_DOTNET}" --list-runtimes OUTPUT_VARIABLE _runtimes ERROR_QUIET)
    string(REGEX MATCHALL "Microsoft\\.NETCore\\.App 10\\.[0-9]+\\.[0-9]+ \\[[^]\n]*\\]" _runtimes "${_runtimes}")
    set(_best "")
    foreach(_line IN LISTS _runtimes)
        string(REGEX REPLACE "^Microsoft\\.NETCore\\.App ([0-9.]+) \\[(.*)\\]$" "\\1" _version "${_line}")
        string(REGEX REPLACE "^Microsoft\\.NETCore\\.App ([0-9.]+) \\[(.*)\\]$" "\\2" _dir "${_line}")
        if(_best STREQUAL "" OR _version VERSION_GREATER _best)
            set(_best "${_version}")
            file(TO_CMAKE_PATH "${_dir}/${_version}" _dotnet_framework)
        endif()
    endforeach()
endif()
if(_dotnet_framework AND EXISTS "${_dotnet_framework}")
    get_filename_component(_dotnet_root "${_dotnet_framework}/../../.." ABSOLUTE)
    file(GLOB _fxrs LIST_DIRECTORIES true "${_dotnet_root}/host/fxr/*")
    set(_fxr "")
    foreach(_dir IN LISTS _fxrs)
        get_filename_component(_name "${_dir}" NAME)
        get_filename_component(_best_name "${_fxr}" NAME)
        if(_fxr STREQUAL "" OR _name VERSION_GREATER _best_name)
            set(_fxr "${_dir}")
        endif()
    endforeach()
    get_filename_component(_framework_name "${_dotnet_framework}" NAME)
    get_filename_component(_fxr_name "${_fxr}" NAME)
    message(STATUS "Installers bundle the .NET runtime ${_framework_name} (host ${_fxr_name}) from ${_dotnet_root}")
    install(DIRECTORY "${_fxr}/" DESTINATION "bin/dotnet/host/fxr/${_fxr_name}" COMPONENT Runtime)
    install(DIRECTORY "${_dotnet_framework}/" DESTINATION "bin/dotnet/shared/Microsoft.NETCore.App/${_framework_name}" COMPONENT Runtime)
    foreach(_license LICENSE.txt ThirdPartyNotices.txt)
        if(EXISTS "${_dotnet_root}/${_license}")
            install(FILES "${_dotnet_root}/${_license}" DESTINATION bin/dotnet COMPONENT Runtime)
        endif()
    endforeach()
else()
    message(WARNING "No .NET 10 runtime found: the installers won't bundle one (C# scripts will need .NET 10 installed)")
endif()

# The Microsoft C++ runtime next to the programs (no separate redistributable to install).
if(WIN32)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT Runtime)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS ON) # (the fallback below covers what it misses)
    include(InstallRequiredSystemLibraries)
    # (CMake versions that don't know this Visual Studio find nothing: take the CRT from the redist folder the MSVC
    # environment names, which matches the compiler)
    if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS AND MSVC)
        file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}" _redist)
        file(GLOB _crt "${_redist}/x64/Microsoft.VC*.CRT/*.dll")
        if(_crt)
            install(FILES ${_crt} DESTINATION bin COMPONENT Runtime)
        else()
            message(WARNING "The Microsoft C++ runtime DLLs weren't found: installs will need the VC++ redistributable")
        endif()
    endif()
endif()

# The Siege sample: the files its scenes need (Projects/Siege/Sample.files, made by its Tools/sample_files.py: the
# scenes and everything they reference, the scripts, the credits), not the source assets nothing uses, nor the tools
# or what the editor makes in it (caches, script builds, Directory.Build.props with this machine's paths).
set(_sample "${CMAKE_SOURCE_DIR}/Projects/Siege")
if(EXISTS "${_sample}/Sample.files")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sample}/Sample.files")
    file(STRINGS "${_sample}/Sample.files" _sample_files ENCODING UTF-8)
    foreach(_rel IN LISTS _sample_files)
        if(_rel STREQUAL "" OR _rel MATCHES "^#")
            continue()
        endif()
        if(NOT EXISTS "${_sample}/${_rel}")
            message(WARNING "The Siege sample's ${_rel} is missing (run Projects/Siege/Tools/sample_files.py)")
            continue()
        endif()
        get_filename_component(_dir "${_rel}" DIRECTORY)
        install(FILES "${_sample}/${_rel}" DESTINATION "Samples/Siege/${_dir}" COMPONENT Runtime)
    endforeach()
    # Its C# scripts compiled ahead of time (.dimension/ScriptAssemblies, newer than the sources so the editor and the
    # player load them as they are): it plays on a machine without the .NET SDK. Built from a copy of the installed
    # Scripts folder outside the install (no bin/obj left in it), against the installed engine assemblies.
    if(DIM_DOTNET)
        install(CODE "
            set(_siege \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/Samples/Siege\")
            set(_work \"${CMAKE_BINARY_DIR}/siege-scripts\")
            file(REMOVE_RECURSE \"\${_work}\")
            file(COPY \"\${_siege}/Scripts/\" DESTINATION \"\${_work}\")
            message(STATUS \"Compiling the Siege sample's C# scripts\")
            execute_process(COMMAND \"${DIM_DOTNET}\" build \"\${_work}/GameScripts.csproj\" -c Release
                                    -o \"\${_siege}/.dimension/ScriptAssemblies\" --nologo -v q
                                    \"-p:DimensionManagedDir=\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/managed\"
                            RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
            if(NOT _result EQUAL 0)
                message(FATAL_ERROR \"Compiling the Siege sample's scripts failed:\\n\${_out}\")
            endif()
            file(GLOB _pdbs \"\${_siege}/.dimension/ScriptAssemblies/*.pdb\")
            if(_pdbs)
                file(REMOVE \${_pdbs})
            endif()
        " COMPONENT Runtime)
    else()
        message(WARNING "dotnet not found: the Siege sample ships uncompiled (it needs the .NET SDK to play)")
    endif()
    # Its textures compressed ahead of time (Prebuilt/Textures, keyed by the files' contents): the first open reads
    # them instead of compressing them (about 12 s). Made from the installed files by the player, which needs no GPU
    # for it; ~105 MB more in the install.
    install(CODE "
        set(_siege \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/Samples/Siege\")
        message(STATUS \"Prebuilding the Siege sample's compressed textures\")
        execute_process(COMMAND \"$<TARGET_FILE:DimensionPlayer>\" \"\${_siege}\" --prebuild-textures \"\${_siege}/Prebuilt/Textures\"
                        RESULT_VARIABLE _result OUTPUT_QUIET ERROR_QUIET)
        if(NOT _result EQUAL 0)
            message(WARNING \"Prebuilding the Siege sample's textures failed (\${_result}): it compresses them when first opened\")
        endif()
    " COMPONENT Runtime)
else()
    message(WARNING "Projects/Siege/Sample.files is missing: the installers ship no sample")
endif()

if(NOT WIN32)
    install(FILES "${CMAKE_SOURCE_DIR}/Engine/Resources/Dimension.png" DESTINATION share/icons RENAME dimension-editor.png COMPONENT Runtime)
    install(FILES "${CMAKE_SOURCE_DIR}/cmake/packaging/dimension-editor.desktop" DESTINATION share/applications COMPONENT Runtime)
    install(PROGRAMS "${CMAKE_SOURCE_DIR}/cmake/packaging/install.sh" DESTINATION . COMPONENT Runtime)
endif()

# ---- CPack

set(CPACK_PACKAGE_NAME "DimensionEngine")
set(CPACK_PACKAGE_VENDOR "Dimension")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Dimension Engine: a 3D game engine and editor")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Dimension Engine")
set(CPACK_PACKAGE_EXECUTABLES "DimensionEditor" "Dimension Editor")
set(CPACK_STRIP_FILES ON)
set(CPACK_PACKAGE_CHECKSUM SHA256)

if(WIN32)
    set(CPACK_GENERATOR "NSIS;ZIP")
    set(CPACK_PACKAGE_FILE_NAME "DimensionEngine-${PROJECT_VERSION}-Windows-x64")
    set(CPACK_NSIS_PACKAGE_NAME "Dimension Engine ${PROJECT_VERSION}")
    set(CPACK_NSIS_DISPLAY_NAME "Dimension Engine")
    set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/Engine/Resources/Dimension.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/Engine/Resources/Dimension.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\DimensionEditor.exe")
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY "bin")
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "DimensionEditor.exe")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
    set(CPACK_NSIS_MODIFY_PATH OFF)
    # A desktop shortcut, always. (CPACK_CREATE_DESKTOP_LINKS only acts on a checkbox of the install-options page,
    # which CPack shows just when it offers to change PATH: off here, so it never made one.)
    # (four backslashes: this file and the generated CPackConfig.cmake each halve them)
    set(CPACK_NSIS_CREATE_ICONS_EXTRA "CreateShortCut '$DESKTOP\\\\Dimension Editor.lnk' '$INSTDIR\\\\bin\\\\DimensionEditor.exe'")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA "Delete '$DESKTOP\\\\Dimension Editor.lnk'")
    set(CPACK_NSIS_UNINSTALL_NAME "Uninstall Dimension Engine")
    set(CPACK_NSIS_BRANDING_TEXT "Dimension Engine ${PROJECT_VERSION}")
    set(CPACK_NSIS_MANIFEST_DPI_AWARE ON)
else()
    set(CPACK_GENERATOR "DEB;TGZ")
    set(CPACK_PACKAGE_FILE_NAME "DimensionEngine-${PROJECT_VERSION}-Linux-x86_64")
    # The .deb puts the tree in /opt/dimension (its scripts add the menu entry and the command); the tarball unpacks
    # anywhere (its install.sh does the same for one user or system-wide).
    set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/dimension")
    set(CPACK_DEB_COMPONENT_INSTALL OFF)
    set(CPACK_DEBIAN_FILE_NAME "dimension-engine_${PROJECT_VERSION}_amd64.deb")
    set(CPACK_DEBIAN_PACKAGE_NAME "dimension-engine")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Dimension")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    # (loaded at run time, so shlibdeps can't see them: Vulkan; file dialogs; C# scripts)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libvulkan1")
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "zenity, mesa-vulkan-drivers")
    set(CPACK_DEBIAN_PACKAGE_SUGGESTS "dotnet-sdk-10.0")
    # (maintainer scripts must be executable: copied with that mode, whatever the checkout gave them)
    file(COPY "${CMAKE_SOURCE_DIR}/cmake/packaging/postinst" "${CMAKE_SOURCE_DIR}/cmake/packaging/prerm"
         DESTINATION "${CMAKE_BINARY_DIR}/packaging"
         FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_BINARY_DIR}/packaging/postinst;${CMAKE_BINARY_DIR}/packaging/prerm")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
    # The tarball: one top folder.
    set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_SOURCE_DIR}/cmake/packaging/CPackOptions.cmake")
endif()

# Only what this file installs (the Runtime component): not the headers and static libraries that bundled libraries'
# own install rules add (box2d, ogg, opus).
set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${PROJECT_NAME};Runtime;/")

include(CPack)
