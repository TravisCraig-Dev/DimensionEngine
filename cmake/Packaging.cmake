# Installers: `cmake --preset dist`, `cmake --build --preset dist`, then `cpack` in out/build/dist.
#   Windows: an NSIS setup (Start menu and desktop shortcuts, uninstaller) and a portable .zip.
#   Linux:   a .deb (installs to /opt/dimension, adds a menu entry and `dimension-editor`) and a .tar.gz with install.sh.
# The install tree is the same everywhere: bin/ (programs, the engine library, the game module, the C# assemblies),
# Samples/Sandbox (the sample project; the editor copies it to the user's Documents to open it) and share/ (icon and
# desktop entry, Linux).

include(GNUInstallDirs)

# Programs and libraries, all in bin/ (the editor finds the player and game module next to itself). The built files
# themselves (not install(TARGETS), which would want the Windows import libraries too); Linux's are linked with
# RPATH $ORIGIN already.
install(PROGRAMS $<TARGET_FILE:DimensionEditor> $<TARGET_FILE:DimensionPlayer> $<TARGET_FILE:DimensionGame>
        DESTINATION bin COMPONENT Runtime)
install(FILES $<TARGET_FILE:DimensionEngine> $<TARGET_FILE:SandboxGame> DESTINATION bin COMPONENT Runtime)
# C# scripting: the host library and the engine's managed assemblies (scripts compile against bin/managed).
if(WIN32)
    install(FILES "${CMAKE_BINARY_DIR}/bin/nethost.dll" DESTINATION bin OPTIONAL COMPONENT Runtime)
else()
    install(FILES "${CMAKE_BINARY_DIR}/bin/libnethost.so" DESTINATION bin OPTIONAL COMPONENT Runtime)
endif()
install(DIRECTORY "${CMAKE_BINARY_DIR}/bin/managed/" DESTINATION bin/managed OPTIONAL COMPONENT Runtime
        PATTERN "*.pdb" EXCLUDE)

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

# The Sandbox sample, without what the editor makes in it (its caches, script builds, Directory.Build.props with this
# machine's paths). From a git checkout, only the files git tracks: work in progress in the sample folder stays out.
set(_sandbox "${CMAKE_SOURCE_DIR}/Projects/Sandbox")
set(_sandbox_files "")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -c core.quotepath=off ls-files -- Projects/Sandbox
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE _tracked RESULT_VARIABLE _git_result
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_git_result EQUAL 0 AND _tracked)
        string(REPLACE "\n" ";" _sandbox_files "${_tracked}")
        # (configure again when the tracked files change)
        if(EXISTS "${CMAKE_SOURCE_DIR}/.git/index")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/.git/index")
        endif()
    endif()
endif()
if(_sandbox_files)
    foreach(_file IN LISTS _sandbox_files)
        file(RELATIVE_PATH _rel "${_sandbox}" "${CMAKE_SOURCE_DIR}/${_file}")
        if(_rel MATCHES "(^|/)(\\.dimension|Scripts/bin|Scripts/obj)/" OR _rel MATCHES "Directory\\.Build\\.props$"
           OR NOT EXISTS "${_sandbox}/${_rel}")
            continue()
        endif()
        get_filename_component(_dir "${_rel}" DIRECTORY)
        install(FILES "${_sandbox}/${_rel}" DESTINATION "Samples/Sandbox/${_dir}" COMPONENT Runtime)
    endforeach()
else()
    install(DIRECTORY "${_sandbox}/" DESTINATION Samples/Sandbox COMPONENT Runtime
            PATTERN ".dimension" EXCLUDE
            PATTERN "Scripts/bin" EXCLUDE
            PATTERN "Scripts/obj" EXCLUDE
            PATTERN "Directory.Build.props" EXCLUDE)
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
# Only this project's Runtime component (the fetched libraries' own install rules add headers and static libraries).
set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${PROJECT_NAME};Runtime;/")

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

include(CPack)
