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
    bench_test.cpp
    journal_test.cpp
    recovery_test.cpp)
target_link_libraries(enginex_tests PRIVATE enginex_codec enginex_bench_lib
    GTest::gtest_main)
target_include_directories(enginex_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(enginex_tests PRIVATE
    ENGINEX_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    ENGINEX_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
enginex_set_warnings(enginex_tests)

include(GoogleTest)
gtest_discover_tests(enginex_tests DISCOVERY_TIMEOUT 60)

# End-to-end CLI checks that need a real process: kill -9 during journaled sessions, and
# corruption/tail handling through `enginex replay` (tools/fault_injection.py).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME cli_fault_injection_kill9
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/fault_injection.py
                     --enginex $<TARGET_FILE:enginex> --rounds 6 --seed 7)
    set_tests_properties(cli_fault_injection_kill9 PROPERTIES TIMEOUT 300 LABELS "recovery;cli")
endif()
