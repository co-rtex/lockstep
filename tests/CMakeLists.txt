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
    protocol_fuzz_test.cpp
    siphash_test.cpp)
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
    # Independent Python model of matching and command rules vs `enginex simulate`.
    add_test(NAME exchange_pymodel_differential
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/exchange_pymodel.py
                     --seeds 10 --commands 2000 --enginex $<TARGET_FILE:enginex>)
    set_tests_properties(exchange_pymodel_differential PROPERTIES TIMEOUT 120 LABELS "cli")
    # The same model over TCP with a journal: serve, submit, restart on the journal, resubmit.
    add_test(NAME exchange_pymodel_network_restart
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/exchange_pymodel.py
                     --restart --seeds 5 --commands 1000 --enginex $<TARGET_FILE:enginex>)
    set_tests_properties(exchange_pymodel_network_restart PROPERTIES TIMEOUT 180 LABELS "cli;net;recovery")
    # Eight concurrent clients, journaled: the server's interleaving must be linearizable
    # against the model (every reply, every feed event, final books).
    add_test(NAME exchange_pymodel_concurrent
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/exchange_pymodel.py
                     --clients 8 --journal --seeds 3 --commands 2000 --enginex $<TARGET_FILE:enginex>)
    set_tests_properties(exchange_pymodel_concurrent PROPERTIES TIMEOUT 180 LABELS "cli;net")
    # The same with hostile connections alongside (garbage, bad lengths, half frames, malformed
    # JSON, stalled subscribers, abrupt drops): server survives, every malformed message gets
    # INVALID_REQUEST, the honest traffic stays linearizable, the journal replays to the same hash.
    add_test(NAME exchange_pymodel_chaos
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/exchange_pymodel.py
                     --clients 8 --journal --chaos 4 --seeds 2 --commands 5000
                     --enginex $<TARGET_FILE:enginex>)
    set_tests_properties(exchange_pymodel_chaos PROPERTIES TIMEOUT 240 LABELS "cli;net")
    # The performance acceptance gate (tools/paired_ab.py) against fake benches: verdicts,
    # minimum effect, pair minimum and the correctness refusal.
    add_test(NAME paired_ab_gate
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/paired_ab_test.py)
    set_tests_properties(paired_ab_gate PROPERTIES TIMEOUT 120 LABELS "tools")
endif()

# ITCH CLI and fixture checks: the committed golden fixtures match their generator, a strict
# replay of the lifecycle fixture succeeds, and the malformed fixture exits non-zero.
if(Python3_Interpreter_FOUND)
    add_test(NAME itch_fixtures_up_to_date
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/itch_fixture.py --check)
    set_tests_properties(itch_fixtures_up_to_date PROPERTIES LABELS "itch")
    # Independent Python decoder (tools/itch_pydecode.py) vs `itch dump --all-fields`: both
    # fixtures, and 200 random messages of each of the 23 types with every field random.
    add_test(NAME itch_pydecode_lifecycle
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/itch_pydecode.py
                     ${CMAKE_CURRENT_SOURCE_DIR}/testdata/itch/lifecycle.itch
                     --enginex $<TARGET_FILE:enginex>)
    add_test(NAME itch_pydecode_malformed
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/itch_pydecode.py
                     ${CMAKE_CURRENT_SOURCE_DIR}/testdata/itch/malformed.itch
                     --enginex $<TARGET_FILE:enginex>)
    add_test(NAME itch_pydecode_random_all_types
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/itch_pydecode.py
                     --random 200 --seed 11 --enginex $<TARGET_FILE:enginex>)
    add_test(NAME itch_pybook_lifecycle
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/itch_pybook.py
                     ${CMAKE_CURRENT_SOURCE_DIR}/testdata/itch/lifecycle.itch
                     --checkpoint-every 1 --enginex $<TARGET_FILE:enginex>)
    set_tests_properties(itch_pydecode_lifecycle itch_pydecode_malformed
                         itch_pydecode_random_all_types itch_pybook_lifecycle
                         PROPERTIES LABELS "itch;cli" TIMEOUT 120)
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
