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
    recovery_test.cpp
    networking_test.cpp
    itch_codec_test.cpp
    itch_book_test.cpp
    itch_fixture_test.cpp
    itch_order_index_test.cpp
    mirror_test.cpp
    itch_moldudp64_test.cpp
    itch_mold_udp_test.cpp
    protocol_fuzz_test.cpp)
target_link_libraries(enginex_tests PRIVATE enginex_net enginex_bench_lib enginex_itch enginex_itch_udp
    GTest::gtest_main)
target_include_directories(enginex_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(enginex_tests PRIVATE
    ENGINEX_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    ENGINEX_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
enginex_set_warnings(enginex_tests)

include(GoogleTest)
gtest_discover_tests(enginex_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)

# End-to-end CLI checks that need a real process: kill -9 during journaled sessions, and
# corruption/tail handling through `enginex replay` (tools/fault_injection.py).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME cli_fault_injection_kill9
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/fault_injection.py
                     --enginex $<TARGET_FILE:enginex> --rounds 6 --seed 7)
    set_tests_properties(cli_fault_injection_kill9 PROPERTIES TIMEOUT 300 LABELS "recovery;cli")
endif()

# ITCH CLI and fixture checks: the committed golden fixtures match their generator, a strict
# replay of the lifecycle fixture succeeds, and the malformed fixture exits non-zero.
if(Python3_Interpreter_FOUND)
    add_test(NAME itch_fixtures_up_to_date
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/itch_fixture.py --check)
    set_tests_properties(itch_fixtures_up_to_date PROPERTIES LABELS "itch")
endif()
add_test(NAME cli_itch_replay_strict
         COMMAND enginex itch replay --strict --json
                 --input ${CMAKE_CURRENT_SOURCE_DIR}/testdata/itch/lifecycle.itch)
add_test(NAME cli_itch_replay_malformed_fails
         COMMAND enginex itch replay --json
                 --input ${CMAKE_CURRENT_SOURCE_DIR}/testdata/itch/malformed.itch)
set_tests_properties(cli_itch_replay_strict cli_itch_replay_malformed_fails
                     PROPERTIES LABELS "itch;cli")
set_tests_properties(cli_itch_replay_malformed_fails PROPERTIES WILL_FAIL TRUE)

# ITCH generate -> replay -> dump through the CLI (exit codes and a deterministic hash).
if(Python3_Interpreter_FOUND)
    add_test(NAME cli_itch_generate_replay_dump
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_itch_smoke.py
                     $<TARGET_FILE:enginex>)
    set_tests_properties(cli_itch_generate_replay_dump PROPERTIES LABELS "itch;cli" TIMEOUT 120)
endif()

# Short, seeded run of the fuzz harness (regression net; long runs are manual, see fuzz/README).
add_test(NAME fuzz_itch_standalone_smoke
         COMMAND enginex_fuzz_itch_standalone 20000 1
                 ${CMAKE_CURRENT_SOURCE_DIR}/testdata/itch/lifecycle.itch
                 ${CMAKE_CURRENT_SOURCE_DIR}/testdata/itch/malformed.itch
                 ${PROJECT_SOURCE_DIR}/fuzz/corpus/mold_packets.bin
                 ${PROJECT_SOURCE_DIR}/fuzz/corpus/mold_gaps.bin)
set_tests_properties(fuzz_itch_standalone_smoke PROPERTIES LABELS "itch;fuzz" TIMEOUT 300)

# Every ITCH book layout variant must reconstruct byte-identical books (D27/D28).
if(Python3_Interpreter_FOUND)
    add_test(NAME itch_layout_variants_agree
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/compare_variant_hashes.py
                     $<TARGET_FILE:enginex_itch_state_hash>
                     $<TARGET_FILE:enginex_itch_state_hash_maphash>)
    set_tests_properties(itch_layout_variants_agree PROPERTIES LABELS "itch" TIMEOUT 300
                         SKIP_RETURN_CODE 77)
endif()
