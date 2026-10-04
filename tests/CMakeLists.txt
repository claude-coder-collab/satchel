include(FetchContent)
FetchContent_Declare(Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.0.tar.gz
    URL_HASH SHA256=0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# The public header must compile as plain C.
add_library(zp_capi_c_check STATIC capi/c_header_check.c)
target_include_directories(zp_capi_c_check PRIVATE ${PROJECT_SOURCE_DIR}/core/include)
set_target_properties(zp_capi_c_check PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

add_executable(zp_tests
    support/pcm_fixtures.cpp
    support/test_support.cpp
    unit/test_archive_metadata.cpp
    unit/test_bytes.cpp
    unit/test_codecs.cpp
    unit/test_flac_format.cpp
    unit/test_hash.cpp
    unit/test_path_policy.cpp
    unit/test_pcm.cpp
    unit/test_planner.cpp
    unit/test_reader.cpp
    unit/test_streams.cpp
    unit/test_zip64_policy.cpp
    unit/test_zip_writer.cpp
    integration/test_build_extract.cpp
    integration/test_capi.cpp
    integration/test_editor.cpp
    integration/test_extract_plan.cpp
    integration/test_filesystem.cpp
    integration/test_flac.cpp
    integration/test_interop.cpp
    integration/test_large.cpp
)
target_include_directories(zp_tests PRIVATE support)
target_link_libraries(zp_tests PRIVATE zp::core zp_capi_c_check Catch2::Catch2WithMain)
target_compile_definitions(zp_tests PRIVATE ZP_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
if(EMSCRIPTEN)
    target_link_options(zp_tests PRIVATE -sPROXY_TO_PTHREAD -sMAXIMUM_MEMORY=4GB -sEXIT_RUNTIME=1 -sNODERAWFS=1 -sPTHREAD_POOL_SIZE=8 -sENVIRONMENT=node)
    add_test(NAME zp_tests COMMAND node $<TARGET_FILE:zp_tests>)
else()
    catch_discover_tests(zp_tests DISCOVERY_MODE PRE_TEST)
endif()
