# Code Mode's C# debugger engine: netcoredbg (Samsung, MIT licence), a .NET debugger the editor drives through the
# Debug Adapter Protocol. A pinned release, downloaded once at configure time (checked against its SHA-256), placed in
# bin/netcoredbg beside the editor and installed with it. Off (or offline): the editor's debugger says it is missing.
option(DIM_NETCOREDBG "Download netcoredbg for Code Mode's C# debugger" ON)
set(DIM_NETCOREDBG_VERSION "3.2.0-1092")
if(DIM_NETCOREDBG AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|amd64")
    if(WIN32)
        set(_ncdbg_file "netcoredbg-win64.zip")
        set(_ncdbg_hash "3c410a45fa502415203a94fcb88654af65bf8e3dac158a5527a722e7a6b9274a")
    else()
        set(_ncdbg_file "netcoredbg-linux-amd64.tar.gz")
        set(_ncdbg_hash "080eb3b2d2152465f599d3b33d1ee6e747794e11cc0a3773ec689f5e5f2c5afa")
    endif()
    set(_ncdbg_dir "${CMAKE_BINARY_DIR}/_deps/netcoredbg-${DIM_NETCOREDBG_VERSION}")
    if(NOT EXISTS "${_ncdbg_dir}/netcoredbg")
        set(_ncdbg_archive "${CMAKE_BINARY_DIR}/_deps/${_ncdbg_file}")
        file(DOWNLOAD "https://github.com/Samsung/netcoredbg/releases/download/${DIM_NETCOREDBG_VERSION}/${_ncdbg_file}" "${_ncdbg_archive}"
             EXPECTED_HASH SHA256=${_ncdbg_hash} STATUS _ncdbg_status)
        list(GET _ncdbg_status 0 _ncdbg_code)
        if(_ncdbg_code EQUAL 0)
            file(ARCHIVE_EXTRACT INPUT "${_ncdbg_archive}" DESTINATION "${_ncdbg_dir}")
            file(DOWNLOAD "https://raw.githubusercontent.com/Samsung/netcoredbg/${DIM_NETCOREDBG_VERSION}/LICENSE" "${_ncdbg_dir}/netcoredbg/LICENSE"
                 EXPECTED_HASH SHA256=6cd03b0de8299b0800f22b35ae842c931ded7684a2d1ba4f1d4188bab9b09a11 STATUS _ncdbg_license)
        else()
            message(WARNING "netcoredbg could not be downloaded (${_ncdbg_status}): Code Mode's debugger will be unavailable")
        endif()
    endif()
    if(EXISTS "${_ncdbg_dir}/netcoredbg")
        message(STATUS "C# debugger: netcoredbg ${DIM_NETCOREDBG_VERSION}")
        add_custom_target(NetCoreDbg ALL
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${_ncdbg_dir}/netcoredbg" "${CMAKE_BINARY_DIR}/bin/netcoredbg"
            COMMENT "Copying netcoredbg (Code Mode's C# debugger)")
        add_dependencies(DimensionEditor NetCoreDbg)
        install(DIRECTORY "${_ncdbg_dir}/netcoredbg/" DESTINATION bin/netcoredbg COMPONENT Runtime USE_SOURCE_PERMISSIONS)
    endif()
endif()
