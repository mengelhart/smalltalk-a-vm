enable_testing()

add_executable(test_public_api test_public_api.c)
target_link_libraries(test_public_api PRIVATE sta_vm)

add_test(NAME test_public_api COMMAND test_public_api)

add_executable(test_objheader_spike test_objheader_spike.c)
target_include_directories(test_objheader_spike PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME objheader_spike COMMAND test_objheader_spike)

# ── Spike 002: MPSC mailbox — TSan correctness gate ───────────────────────
find_package(Threads REQUIRED)

add_executable(test_mailbox_spike
    test_mailbox_spike.c
    ../src/actor/mailbox_spike.c)
target_include_directories(test_mailbox_spike PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_mailbox_spike PRIVATE -fsanitize=thread)
target_link_options(test_mailbox_spike PRIVATE -fsanitize=thread)
target_link_libraries(test_mailbox_spike PRIVATE Threads::Threads)
add_test(NAME mailbox_spike COMMAND test_mailbox_spike)

# ── Spike 002: MPSC mailbox — benchmark build (run manually, not via ctest) ─
add_executable(bench_mailbox_spike
    test_mailbox_spike.c
    ../src/actor/mailbox_spike.c)
target_include_directories(bench_mailbox_spike PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(bench_mailbox_spike PRIVATE STA_BENCH=1)
target_compile_options(bench_mailbox_spike PRIVATE -O2)
target_link_libraries(bench_mailbox_spike PRIVATE Threads::Threads)
