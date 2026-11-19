# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Tessera contributors
#
# UNTESTED: written against the vcpkg documentation, never run through vcpkg (see docs/releasing.md).

# Release ports download the tagged source. A local checkout can be tried with an overlay port:
#   TESSERA_LOCAL_SOURCE=/path/to/Tessera vcpkg install tessera --overlay-ports=packaging/vcpkg/ports
if(DEFINED ENV{TESSERA_LOCAL_SOURCE})
    set(SOURCE_PATH "$ENV{TESSERA_LOCAL_SOURCE}")
else()
    vcpkg_from_github(
        OUT_SOURCE_PATH SOURCE_PATH
        REPO AndreasNilsson123/Tessera
        REF "v${VERSION}"
        SHA512 0 # replace with the real value after tagging: vcpkg prints the expected hash on the first failure
        HEAD_REF master
    )
endif()

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        tools TESSERA_BUILD_TOOLS
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" BUILD_SHARED)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DTESSERA_BUILD_SHARED=${BUILD_SHARED}
        -DTESSERA_BUILD_TESTS=OFF
        -DTESSERA_BUILD_JAVA=OFF
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/Tessera PACKAGE_NAME Tessera)

if("tools" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES tessera-diag AUTO_CLEAN)
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
