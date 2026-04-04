# =============================================================================
# Tests — Catch2 v3
# =============================================================================

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# Helper: define a test executable linked to Catch2 + a whiteout target, and
# auto-register it with CTest.
function(whiteout_add_test TARGET SOURCE)
    cmake_parse_arguments(ARG "" "LINK" "" ${ARGN})
    if(NOT ARG_LINK)
        set(ARG_LINK whiteout_lib)
    endif()
    add_executable(${TARGET} ${SOURCE})
    target_link_libraries(${TARGET} PRIVATE ${ARG_LINK} Catch2::Catch2WithMain)
    catch_discover_tests(${TARGET} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()

# =============================================================================
# Core Tests
# =============================================================================

whiteout_add_test(vertex_buffer_test vertex_buffer_test.cpp)
whiteout_add_test(deflate_test deflate_test.cpp)
target_include_directories(deflate_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_executable(mapped_file_test
    mapped_file_test.cpp
    ${CMAKE_SOURCE_DIR}/src/whiteout/storages/common/mapped_file.cpp
)
target_link_libraries(mapped_file_test PRIVATE whiteout_lib Catch2::Catch2WithMain)
catch_discover_tests(mapped_file_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Texture Tests
whiteout_add_test(bc2_test bc2_test.cpp)
whiteout_add_test(bc7_roundtrip_test bc7_roundtrip_test.cpp)
whiteout_add_test(bc7_block_decode_test bc7_block_decode_test.cpp)
whiteout_add_test(blp0_test blp0_test.cpp)
whiteout_add_test(jpeg_test jpeg_test.cpp)
whiteout_add_test(quantize_test quantize_test.cpp)

# Model Tests
whiteout_add_test(m2_corpus_test m2_corpus_test.cpp)
whiteout_add_test(m2_diag_test m2_diag_test.cpp)

# SNO / Diablo Tests
whiteout_add_test(d3_ani_corpus_test d3_ani_corpus_test.cpp)
whiteout_add_test(d3_app_corpus_test d3_app_corpus_test.cpp)
whiteout_add_test(d4_sno_diag_test d4_sno_diag_test.cpp)
whiteout_add_test(d4_tex_corpus_test d4_tex_corpus_test.cpp)

# MDL Tests
whiteout_add_test(mdl_parser_test mdl_parser_test.cpp)
target_include_directories(mdl_parser_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

whiteout_add_test(mdl_converter_test mdl_converter_test.cpp)
target_include_directories(mdl_converter_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

whiteout_add_test(mdl_writer_test mdl_writer_test.cpp)
target_include_directories(mdl_writer_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

# =============================================================================
# CASC Tests
# =============================================================================
if(WHITEOUT_ENABLE_CASC)

    whiteout_add_test(casc_crypto_test casc_crypto_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_blte_test casc_blte_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_config_test casc_config_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_index_test casc_index_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_encoding_test casc_encoding_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_wow_root_test casc_wow_root_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_d3_root_test casc_d3_root_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_tvfs_root_test casc_tvfs_root_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_ow_root_test casc_ow_root_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_corpus_test casc_corpus_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_parallel_test casc_parallel_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_round_trip_test casc_round_trip_test.cpp LINK whiteout_casc)
    whiteout_add_test(online_storage_test online_storage_test.cpp LINK whiteout_casc)

    add_executable(online_cross_validate_test online_cross_validate_test.cpp)
    target_link_libraries(online_cross_validate_test PRIVATE whiteout_casc Catch2::Catch2WithMain)
    target_include_directories(online_cross_validate_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    catch_discover_tests(online_cross_validate_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_executable(casc_race_test casc_race_test.cpp)
    target_link_libraries(casc_race_test PRIVATE whiteout_casc Catch2::Catch2WithMain)
    target_include_directories(casc_race_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    catch_discover_tests(casc_race_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # Performance heatmap test — standalone executable with custom main().
    # Usage: casc_perf_heatmap_test <casc_path> [output_dir]
    add_executable(casc_perf_heatmap_test casc_perf_heatmap_test.cpp)
    target_link_libraries(casc_perf_heatmap_test PRIVATE whiteout_casc)

    # Performance cross-reference test — standalone executable with custom main().
    # Compares WhiteoutLib CASC against CascLib for every measurable operation.
    # Usage: casc_perf_crossref_test <casc_path> [output_csv]
    add_executable(casc_perf_crossref_test casc_perf_crossref_test.cpp)
    target_link_libraries(casc_perf_crossref_test PRIVATE whiteout_casc)

    # Online performance cross-reference test — standalone executable with custom main().
    # Benchmarks Storage::openOnline (CDN-backed) read performance.
    # Usage: online_perf_crossref_test <product> [options]
    add_executable(online_perf_crossref_test online_perf_crossref_test.cpp)
    target_link_libraries(online_perf_crossref_test PRIVATE whiteout_casc)

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
        target_link_libraries(casc_cross_validate_test PRIVATE whiteout_casc casc_static Catch2::Catch2WithMain)
        target_compile_definitions(casc_cross_validate_test PRIVATE WHITEOUT_HAS_CASCLIB_CROSSVAL=1)
        catch_discover_tests(casc_cross_validate_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

        # Performance cross-reference with CascLib comparison.
        target_link_libraries(casc_perf_crossref_test PRIVATE casc_static)
        target_compile_definitions(casc_perf_crossref_test PRIVATE WHITEOUT_HAS_CASCLIB_CROSSVAL=1)

        # Online performance cross-reference with CascLib comparison.
        target_link_libraries(online_perf_crossref_test PRIVATE casc_static)
        target_compile_definitions(online_perf_crossref_test PRIVATE WHITEOUT_HAS_CASCLIB_CROSSVAL=1)

    else()
        # Build the cross-validate test without CascLib (internal consistency only).
        add_executable(casc_cross_validate_test casc_cross_validate_test.cpp)
        target_link_libraries(casc_cross_validate_test PRIVATE whiteout_casc Catch2::Catch2WithMain)
        catch_discover_tests(casc_cross_validate_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    endif()

endif() # WHITEOUT_ENABLE_CASC

# =============================================================================
# MPQ Tests
# =============================================================================
if(WHITEOUT_ENABLE_MPQ)

    add_executable(mpq_race_test mpq_race_test.cpp)
    target_link_libraries(mpq_race_test PRIVATE whiteout_mpq Catch2::Catch2WithMain)
    target_include_directories(mpq_race_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    catch_discover_tests(mpq_race_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    whiteout_add_test(mpq_corpus_test mpq_corpus_test.cpp LINK whiteout_mpq)
    whiteout_add_test(mpq_test mpq_test.cpp LINK whiteout_mpq)

    add_executable(mpq_codec_test mpq_codec_test.cpp)
    target_link_libraries(mpq_codec_test PRIVATE whiteout_mpq Catch2::Catch2WithMain)
    target_include_directories(mpq_codec_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    catch_discover_tests(mpq_codec_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_executable(mpq_parallel_test mpq_parallel_test.cpp)
    target_link_libraries(mpq_parallel_test PRIVATE whiteout_mpq Catch2::Catch2WithMain)
    target_include_directories(mpq_parallel_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    catch_discover_tests(mpq_parallel_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

endif() # WHITEOUT_ENABLE_MPQ
