# Per-generator CPack settings (CPACK_PROJECT_CONFIG_FILE): read by cpack once for each generator.
# The .deb installs under /opt/dimension; the tarball keeps the tree at its top (bin/, Samples/, share/, install.sh).
if(CPACK_GENERATOR STREQUAL "TGZ")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/")
endif()
