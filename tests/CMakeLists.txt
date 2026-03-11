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

# ── Spike 003: Work-stealing scheduler — TSan correctness gate ────────────
add_executable(test_scheduler_spike
    test_scheduler_spike.c
    ../src/scheduler/scheduler_spike.c)
target_include_directories(test_scheduler_spike PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_scheduler_spike PRIVATE -fsanitize=thread)
target_link_options(test_scheduler_spike PRIVATE -fsanitize=thread)
target_link_libraries(test_scheduler_spike PRIVATE Threads::Threads)
add_test(NAME scheduler_spike COMMAND test_scheduler_spike)
set_tests_properties(scheduler_spike PROPERTIES TIMEOUT 120)

# ── Spike 003: Scheduler — benchmark build (run manually, not via ctest) ───
add_executable(bench_scheduler_spike
    test_scheduler_spike.c
    ../src/scheduler/scheduler_spike.c)
target_include_directories(bench_scheduler_spike PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(bench_scheduler_spike PRIVATE STA_BENCH=1)
target_compile_options(bench_scheduler_spike PRIVATE -O2)
target_link_libraries(bench_scheduler_spike PRIVATE Threads::Threads)

# ── Spike 003: Per-notification-mode benchmark builds ─────────────────────
# Run ./build/tests/bench_scheduler_notif_1 (2, 3) for the three-way table.
foreach(mode 1 2 3)
    add_executable(bench_scheduler_notif_${mode}
        test_scheduler_spike.c
        ../src/scheduler/scheduler_spike.c)
    target_include_directories(bench_scheduler_notif_${mode}
        PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(bench_scheduler_notif_${mode}
        PRIVATE STA_BENCH=1 STA_NOTIF_MODE=${mode})
    target_compile_options(bench_scheduler_notif_${mode} PRIVATE -O2)
    target_link_libraries(bench_scheduler_notif_${mode} PRIVATE Threads::Threads)
endforeach()

# ── Spike 004: Activation frame layout and TCO ────────────────────────────
add_executable(test_frame_spike
    test_frame_spike.c
    ../src/vm/frame_spike.c
    ../src/scheduler/scheduler_spike.c)
target_include_directories(test_frame_spike PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME frame_spike COMMAND test_frame_spike)

# ── Spike 005: Async I/O via libuv ────────────────────────────────────────
if(STA_USE_LIBUV)
    find_package(Threads REQUIRED)

    # TSan correctness gate — registered with ctest
    add_executable(test_io_spike
        test_io_spike.c
        ../src/io/io_spike.c
        ../src/scheduler/scheduler_spike.c)
    target_include_directories(test_io_spike PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_options(test_io_spike PRIVATE -fsanitize=thread -Wall -Wextra)
    target_link_options(test_io_spike PRIVATE -fsanitize=thread)
    target_link_libraries(test_io_spike PRIVATE ${LIBUV_TARGET} Threads::Threads)
    add_test(NAME io_spike COMMAND test_io_spike)
    set_tests_properties(io_spike PROPERTIES TIMEOUT 120)

    # Benchmark build — no TSan, -O2, run manually for latency numbers
    add_executable(bench_io_spike
        test_io_spike.c
        ../src/io/io_spike.c
        ../src/scheduler/scheduler_spike.c)
    target_include_directories(bench_io_spike PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(bench_io_spike PRIVATE STA_BENCH=1)
    target_compile_options(bench_io_spike PRIVATE -O2 -Wall -Wextra)
    target_link_libraries(bench_io_spike PRIVATE ${LIBUV_TARGET} Threads::Threads)
    # Not a ctest target — run manually: ./build/tests/bench_io_spike
endif()

# ── Spike 003: Per-deque-variant benchmark build ───────────────────────────
add_executable(bench_scheduler_deque_b
    test_scheduler_spike.c
    ../src/scheduler/scheduler_spike.c)
target_include_directories(bench_scheduler_deque_b PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(bench_scheduler_deque_b
    PRIVATE STA_BENCH=1 STA_DEQUE_VARIANT=2)
target_compile_options(bench_scheduler_deque_b PRIVATE -O2)
target_link_libraries(bench_scheduler_deque_b PRIVATE Threads::Threads)
