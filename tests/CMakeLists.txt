# =============================================================================
# Tests
# =============================================================================

# VertexBufferBuilder Test
add_executable(vertex_buffer_test vertex_buffer_test.cpp)
target_link_libraries(vertex_buffer_test PRIVATE whiteout_lib)

# MappedFile Test
add_executable(mapped_file_test
    mapped_file_test.cpp
    ${CMAKE_SOURCE_DIR}/src/whiteout/storages/common/mapped_file.cpp
)
target_link_libraries(mapped_file_test PRIVATE whiteout_lib)

# =============================================================================
# CASC Tests
# =============================================================================
if(WHITEOUT_ENABLE_CASC)

    add_executable(casc_crypto_test casc_crypto_test.cpp)
    target_link_libraries(casc_crypto_test PRIVATE whiteout_casc)

    add_executable(casc_blte_test casc_blte_test.cpp)
    target_link_libraries(casc_blte_test PRIVATE whiteout_casc)

    add_executable(casc_config_test casc_config_test.cpp)
    target_link_libraries(casc_config_test PRIVATE whiteout_casc)

    add_executable(casc_index_test casc_index_test.cpp)
    target_link_libraries(casc_index_test PRIVATE whiteout_casc)

    add_executable(casc_encoding_test casc_encoding_test.cpp)
    target_link_libraries(casc_encoding_test PRIVATE whiteout_casc)

    add_executable(casc_wow_root_test casc_wow_root_test.cpp)
    target_link_libraries(casc_wow_root_test PRIVATE whiteout_casc)

    add_executable(casc_d3_root_test casc_d3_root_test.cpp)
    target_link_libraries(casc_d3_root_test PRIVATE whiteout_casc)

    add_executable(casc_tvfs_root_test casc_tvfs_root_test.cpp)
    target_link_libraries(casc_tvfs_root_test PRIVATE whiteout_casc)

    add_executable(casc_corpus_test casc_corpus_test.cpp)
    target_link_libraries(casc_corpus_test PRIVATE whiteout_casc)

    add_executable(casc_parallel_test casc_parallel_test.cpp)
    target_link_libraries(casc_parallel_test PRIVATE whiteout_casc)

    add_executable(casc_round_trip_test casc_round_trip_test.cpp)
    target_link_libraries(casc_round_trip_test PRIVATE whiteout_casc)

    add_executable(casc_race_test casc_race_test.cpp)
    target_link_libraries(casc_race_test PRIVATE whiteout_casc)
    target_include_directories(casc_race_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

    # =========================================================================
    # Optional CascLib cross-validation target
    # Fetches CascLib for byte-identical comparison against the new
    # pure-C++ CASC implementation via the CascLib C API directly.
    # =========================================================================
    option(WHITEOUT_ENABLE_CASCLIB_CROSSVAL
           "Build CascLib-based cross-validation test (requires internet for FetchContent)" OFF)

    if(WHITEOUT_ENABLE_CASCLIB_CROSSVAL)
        include(FetchContent)
        FetchContent_Declare(
            casclib
            GIT_REPOSITORY https://github.com/ladislav-zezula/CascLib.git
            GIT_TAG        master
        )
        # CascLib's CMakeLists.txt uses an old cmake_minimum_required;
        # set the policy compat floor so CMake 4.x doesn't reject it.
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
        # CascLib builds casc_static as its static library target.
        set(CASC_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
        set(CASC_BUILD_STATIC_LIB ON  CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(casclib)

        # Cross-validate test: links new pure-C++ module and CascLib C API directly.
        add_executable(casc_cross_validate_test casc_cross_validate_test.cpp)
        target_link_libraries(casc_cross_validate_test PRIVATE whiteout_casc casc_static)
        target_compile_definitions(casc_cross_validate_test PRIVATE WHITEOUT_HAS_CASCLIB_CROSSVAL=1)

        # Diablo IV CASC cross-validation test.
        add_executable(d4_casc_test ${CMAKE_SOURCE_DIR}/examples/d4_casc_test.cpp)
        target_link_libraries(d4_casc_test PRIVATE whiteout_casc casc_static)
        target_compile_definitions(d4_casc_test PRIVATE WHITEOUT_HAS_CASCLIB_CROSSVAL=1)
    else()
        # Build the cross-validate test without CascLib (internal consistency only).
        add_executable(casc_cross_validate_test casc_cross_validate_test.cpp)
        target_link_libraries(casc_cross_validate_test PRIVATE whiteout_casc)

        # Diablo IV CASC test (internal consistency only).
        add_executable(d4_casc_test ${CMAKE_SOURCE_DIR}/examples/d4_casc_test.cpp)
        target_link_libraries(d4_casc_test PRIVATE whiteout_casc)
    endif()

endif() # WHITEOUT_ENABLE_CASC

# =============================================================================
# MPQ Tests
# =============================================================================
if(WHITEOUT_ENABLE_MPQ)

    add_executable(mpq_race_test mpq_race_test.cpp)
    target_link_libraries(mpq_race_test PRIVATE whiteout_mpq)
    target_include_directories(mpq_race_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

    add_executable(mpq_corpus_test mpq_corpus_test.cpp)
    target_link_libraries(mpq_corpus_test PRIVATE whiteout_mpq)

endif() # WHITEOUT_ENABLE_MPQ
