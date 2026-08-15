# SPDX-License-Identifier: BSD-3-Clause
#
# vcpkg overlay port for WhiteoutLib.
#
# Drop this directory under a vcpkg overlay (`vcpkg install --overlay-ports=./ports whiteoutlib`)
# or publish a custom registry pointing at it. The REF/SHA512 below pin a
# specific upstream commit — bump both when releasing a new version.

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO            fsahmkow/WhiteoutLib
    REF             v${VERSION}
    SHA512          0
    HEAD_REF        main
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        casc     WHITEOUT_ENABLE_CASC
        mpq      WHITEOUT_ENABLE_MPQ
        database WHITEOUT_ENABLE_DATABASE
        curl     WHITEOUT_ENABLE_CURL_HTTP
        zlib-ng  WHITEOUT_USE_ZLIBNG
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        # When the zlib-ng feature is on, use vcpkg's zlib-ng (pulled in as that
        # feature's dependency) rather than the bundled submodule, which is
        # absent from the GitHub source archive. Inert when the feature is off.
        -DWHITEOUT_USE_SYSTEM_ZLIBNG=ON
        # Bindings are for the upstream FFI consumers (Python/Java/WASM);
        # vcpkg users want the C++ library only, so disable everything else.
        -DWHITEOUT_BUILD_EXAMPLES=OFF
        -DWHITEOUT_BUILD_TESTS=OFF
        -DWHITEOUT_BUILD_PYTHON_BINDINGS=OFF
        -DWHITEOUT_BUILD_C_BINDINGS=OFF
        -DWHITEOUT_BUILD_JNI_BINDINGS=OFF
        -DWHITEOUT_BUILD_WASM_BINDINGS=OFF
        -DWHITEOUT_INSTALL=ON
        # The project pins min CMake to 3.15 but uses newer features in
        # a back-compat path — let vcpkg's modern CMake go through.
        -DCMAKE_POLICY_VERSION_MINIMUM=3.15
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME WhiteoutLib CONFIG_PATH lib/cmake/WhiteoutLib)
vcpkg_copy_pdbs()

# License + attribution.
file(INSTALL "${SOURCE_PATH}/LICENSE"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
    RENAME copyright)

# Headers are the library's only public interface — remove the per-config
# debug/release duplicates vcpkg's check expects to find under share/.
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)
