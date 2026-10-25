add_executable(enginex_tests
    types_test.cpp
    sha256_test.cpp
    matching_test.cpp
    cancel_test.cpp
    state_test.cpp
    canonical_test.cpp
    invariants_test.cpp
    json_codec_test.cpp
    fixtures_test.cpp
    bench_test.cpp)
target_link_libraries(enginex_tests PRIVATE enginex_core enginex_codec enginex_bench_lib
    GTest::gtest_main)
target_include_directories(enginex_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(enginex_tests PRIVATE
    ENGINEX_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    ENGINEX_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
enginex_set_warnings(enginex_tests)

include(GoogleTest)
gtest_discover_tests(enginex_tests DISCOVERY_TIMEOUT 60)
