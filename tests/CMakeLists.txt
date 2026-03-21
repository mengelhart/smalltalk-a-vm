enable_testing()

add_executable(test_public_api test_public_api.c)
target_link_libraries(test_public_api PRIVATE sta_vm)

add_test(NAME test_public_api COMMAND test_public_api)

add_executable(test_objheader_spike test_objheader_spike.c)
target_include_directories(test_objheader_spike PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME objheader_spike COMMAND test_objheader_spike)

# ── Phase 1 Epic 1: Object Memory ──────────────────────────────────────
add_executable(test_oop test_oop.c)
target_include_directories(test_oop PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_oop PRIVATE sta_vm)
add_test(NAME oop COMMAND test_oop)

add_executable(test_immutable_space test_immutable_space.c)
target_include_directories(test_immutable_space PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_immutable_space PRIVATE sta_vm)
add_test(NAME immutable_space COMMAND test_immutable_space)

add_executable(test_heap test_heap.c)
target_include_directories(test_heap PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_heap PRIVATE sta_vm)
add_test(NAME heap COMMAND test_heap)

add_executable(test_special_objects test_special_objects.c)
target_include_directories(test_special_objects PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_special_objects PRIVATE sta_vm)
add_test(NAME special_objects COMMAND test_special_objects)

add_executable(test_class_table test_class_table.c)
target_include_directories(test_class_table PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_class_table PRIVATE sta_vm)
add_test(NAME class_table COMMAND test_class_table)

# ── Phase 1 Epic 2: Symbol Table and Method Dictionary ─────────────────
add_executable(test_symbol_table test_symbol_table.c)
target_include_directories(test_symbol_table PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_symbol_table PRIVATE sta_vm)
add_test(NAME symbol_table COMMAND test_symbol_table)

add_executable(test_method_dict test_method_dict.c)
target_include_directories(test_method_dict PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_method_dict PRIVATE sta_vm)
add_test(NAME method_dict COMMAND test_method_dict)

# ── Phase 1 Epic 3: Bytecode Interpreter Core ─────────────────────────────
add_executable(test_compiled_method test_compiled_method.c)
target_include_directories(test_compiled_method PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_compiled_method PRIVATE sta_vm)
add_test(NAME compiled_method COMMAND test_compiled_method)

add_executable(test_frame test_frame.c)
target_include_directories(test_frame PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_frame PRIVATE sta_vm)
add_test(NAME frame COMMAND test_frame)

add_executable(test_selector test_selector.c)
target_include_directories(test_selector PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_selector PRIVATE sta_vm)
add_test(NAME selector COMMAND test_selector)

add_executable(test_primitive_table test_primitive_table.c)
target_include_directories(test_primitive_table PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_primitive_table PRIVATE sta_vm)
add_test(NAME primitive_table COMMAND test_primitive_table)

add_executable(test_interpreter test_interpreter.c)
target_include_directories(test_interpreter PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_interpreter PRIVATE sta_vm)
add_test(NAME interpreter COMMAND test_interpreter)

# ── Phase 1 Epic 4: Bootstrap ────────────────────────────────────────────
add_executable(test_bootstrap test_bootstrap.c)
target_include_directories(test_bootstrap PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_bootstrap PRIVATE sta_vm)
add_test(NAME bootstrap COMMAND test_bootstrap)

# ── Phase 1 Epic 5: Object Creation ─────────────────────────────────────
add_executable(test_format test_format.c)
target_include_directories(test_format PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_format PRIVATE sta_vm)
add_test(NAME format COMMAND test_format)

add_executable(test_basic_new test_basic_new.c)
target_include_directories(test_basic_new PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_basic_new PRIVATE sta_vm)
add_test(NAME basic_new COMMAND test_basic_new)

add_executable(test_basic_new_size test_basic_new_size.c)
target_include_directories(test_basic_new_size PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_basic_new_size PRIVATE sta_vm)
add_test(NAME basic_new_size COMMAND test_basic_new_size)

add_executable(test_new_methods test_new_methods.c)
target_include_directories(test_new_methods PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_new_methods PRIVATE sta_vm)
add_test(NAME new_methods COMMAND test_new_methods)

add_executable(test_object_creation test_object_creation.c)
target_include_directories(test_object_creation PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_object_creation PRIVATE sta_vm)
add_test(NAME object_creation COMMAND test_object_creation)

# ── Phase 1 Epic 6: Object and Memory Primitives ────────────────────────
add_executable(test_object_memory_prims test_object_memory_prims.c)
target_include_directories(test_object_memory_prims PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_object_memory_prims PRIVATE sta_vm)
add_test(NAME object_memory_prims COMMAND test_object_memory_prims)

# ── Phase 1 Epic 7: Compiler — Scanner and Parser ────────────────────────
add_executable(test_scanner test_scanner.c)
target_include_directories(test_scanner PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_scanner PRIVATE sta_vm)
add_test(NAME scanner COMMAND test_scanner)

add_executable(test_parser test_parser.c)
target_include_directories(test_parser PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_parser PRIVATE sta_vm)
add_test(NAME parser COMMAND test_parser)

# ── Phase 1 Epic 7b: Compiler — Codegen and Integration ───────────────
add_executable(test_codegen test_codegen.c)
target_include_directories(test_codegen PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_codegen PRIVATE sta_vm)
add_test(NAME codegen COMMAND test_codegen)

add_executable(test_compiler_integration test_compiler_integration.c)
target_include_directories(test_compiler_integration PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_compiler_integration PRIVATE sta_vm)
add_test(NAME compiler_integration COMMAND test_compiler_integration)

# ── Phase 1 Epic 8: Exception Handling ───────────────────────────────
add_executable(test_handler test_handler.c)
target_include_directories(test_handler PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_handler PRIVATE sta_vm)
add_test(NAME handler COMMAND test_handler)

add_executable(test_exceptions test_exceptions.c)
target_include_directories(test_exceptions PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_exceptions PRIVATE sta_vm)
add_test(NAME exceptions COMMAND test_exceptions)

# ── Phase 1 Epic 9: Kernel Source Loading ─────────────────────────────────
add_executable(test_class_creation test_class_creation.c)
target_include_directories(test_class_creation PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_class_creation PRIVATE sta_vm)
add_test(NAME class_creation COMMAND test_class_creation)

add_executable(test_filein test_filein.c)
target_include_directories(test_filein PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_filein PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(test_filein PRIVATE sta_vm)
add_test(NAME filein COMMAND test_filein)

add_executable(test_load_source test_load_source.c)
target_include_directories(test_load_source PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_load_source PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(test_load_source PRIVATE sta_vm)
add_test(NAME load_source COMMAND test_load_source)

add_executable(test_smoke_filein test_smoke_filein.c)
target_include_directories(test_smoke_filein PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_smoke_filein PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(test_smoke_filein PRIVATE sta_vm)
add_test(NAME smoke_filein COMMAND test_smoke_filein)

add_executable(test_kernel_load test_kernel_load.c)
target_include_directories(test_kernel_load PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_kernel_load PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_kernel_load PRIVATE sta_vm)
add_test(NAME kernel_load COMMAND test_kernel_load)

add_executable(test_kernel_magnitude test_kernel_magnitude.c)
target_include_directories(test_kernel_magnitude PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_kernel_magnitude PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_kernel_magnitude PRIVATE sta_vm)
add_test(NAME kernel_magnitude COMMAND test_kernel_magnitude)

add_executable(test_kernel_collections test_kernel_collections.c)
target_include_directories(test_kernel_collections PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_kernel_collections PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_kernel_collections PRIVATE sta_vm)
add_test(NAME kernel_collections COMMAND test_kernel_collections)

# ── Phase 1.5 Batch 1: Arithmetic Completion ───────────────────────────
add_executable(test_arithmetic_prims test_arithmetic_prims.c)
target_link_libraries(test_arithmetic_prims PRIVATE sta_vm)
add_test(NAME arithmetic_prims COMMAND test_arithmetic_prims)

add_executable(test_batch1_integration test_batch1_integration.c)
target_link_libraries(test_batch1_integration PRIVATE sta_vm)
add_test(NAME batch1_integration COMMAND test_batch1_integration)

# ── Phase 1.5 Batch 2: Byte-Indexable, Character, String ──────────────
add_executable(test_byte_prims test_byte_prims.c)
target_link_libraries(test_byte_prims PRIVATE sta_vm)
add_test(NAME byte_prims COMMAND test_byte_prims)

add_executable(test_batch2_integration test_batch2_integration.c)
target_link_libraries(test_batch2_integration PRIVATE sta_vm)
add_test(NAME batch2_integration COMMAND test_batch2_integration)

# ── Phase 1.5 Batch 3: Collections ────────────────────────────────────
add_executable(test_replace_prim test_replace_prim.c)
target_link_libraries(test_replace_prim PRIVATE sta_vm)
add_test(NAME replace_prim COMMAND test_replace_prim)

add_executable(test_batch3_collections test_batch3_collections.c)
target_link_libraries(test_batch3_collections PRIVATE sta_vm)
add_test(NAME batch3_collections COMMAND test_batch3_collections)

# ── Phase 1.5 Batch 4: Number Protocol, Symbol, and Hashing ─────────────
add_executable(test_batch4_symbol test_batch4_symbol.c)
target_link_libraries(test_batch4_symbol PRIVATE sta_vm)
add_test(NAME batch4_symbol COMMAND test_batch4_symbol)

add_executable(test_batch4_hash test_batch4_hash.c)
target_link_libraries(test_batch4_hash PRIVATE sta_vm)
add_test(NAME batch4_hash COMMAND test_batch4_hash)

add_executable(test_batch4_number test_batch4_number.c)
target_link_libraries(test_batch4_number PRIVATE sta_vm)
add_test(NAME batch4_number COMMAND test_batch4_number)

add_executable(test_batch4_integration test_batch4_integration.c)
target_link_libraries(test_batch4_integration PRIVATE sta_vm)
add_test(NAME batch4_integration COMMAND test_batch4_integration)

# ── Phase 1.5 Batch 5: Integration Stress Tests ─────────────────────────
add_executable(test_stress_dispatch test_stress_dispatch.c)
target_link_libraries(test_stress_dispatch PRIVATE sta_vm)
add_test(NAME stress_dispatch COMMAND test_stress_dispatch)

add_executable(test_stress_depth test_stress_depth.c)
target_link_libraries(test_stress_depth PRIVATE sta_vm)
add_test(NAME stress_depth COMMAND test_stress_depth)

add_executable(test_stress_exceptions test_stress_exceptions.c)
target_link_libraries(test_stress_exceptions PRIVATE sta_vm)
add_test(NAME stress_exceptions COMMAND test_stress_exceptions)

add_executable(test_stress_strings test_stress_strings.c)
target_link_libraries(test_stress_strings PRIVATE sta_vm)
add_test(NAME stress_strings COMMAND test_stress_strings)

# ── Phase 1 Epic 10: Image Save/Load ────────────────────────────────────
add_executable(test_image_format test_image_format.c)
target_include_directories(test_image_format PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_image_format PRIVATE sta_vm)
add_test(NAME image_format COMMAND test_image_format)

add_executable(test_image_save test_image_save.c)
target_include_directories(test_image_save PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_image_save PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_image_save PRIVATE sta_vm)
add_test(NAME image_save COMMAND test_image_save)

add_executable(test_image_roundtrip test_image_roundtrip.c)
target_include_directories(test_image_roundtrip PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_image_roundtrip PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_image_roundtrip PRIVATE sta_vm)
add_test(NAME image_roundtrip COMMAND test_image_roundtrip)

# ── Phase 2 Epic 0: Handle Table ──────────────────────────────────────
add_executable(test_handle_table test_handle_table.c)
target_include_directories(test_handle_table PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_handle_table PRIVATE sta_vm)
add_test(NAME handle_table COMMAND test_handle_table)

add_executable(test_epic0_verify test_epic0_verify.c)
target_link_libraries(test_epic0_verify PRIVATE sta_vm)
add_test(NAME epic0_verify COMMAND test_epic0_verify)

# ── Phase 1 Epic 11: VM Lifecycle ─────────────────────────────────────
add_executable(test_vm_lifecycle test_vm_lifecycle.c)
target_include_directories(test_vm_lifecycle PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_vm_lifecycle PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_vm_lifecycle PRIVATE sta_vm)
add_test(NAME vm_lifecycle COMMAND test_vm_lifecycle)

add_executable(test_capstone test_capstone.c)
target_link_libraries(test_capstone PRIVATE sta_vm)
add_test(NAME capstone COMMAND test_capstone)

# ── Phase 2 Epic 1: Closures ──────────────────────────────────────────────
add_executable(test_closures test_closures.c)
target_link_libraries(test_closures PRIVATE sta_vm)
add_test(NAME closures COMMAND test_closures)

# ── Phase 2 Epic 2: Actor Struct ──────────────────────────────────────────
add_executable(test_actor_lifecycle test_actor_lifecycle.c)
target_include_directories(test_actor_lifecycle PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_actor_lifecycle PRIVATE sta_vm)
add_test(NAME actor_lifecycle COMMAND test_actor_lifecycle)

add_executable(test_actor_heap test_actor_heap.c)
target_include_directories(test_actor_heap PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_actor_heap PRIVATE sta_vm)
add_test(NAME actor_heap COMMAND test_actor_heap)

add_executable(test_actor_epic2 test_actor_epic2.c)
target_include_directories(test_actor_epic2 PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_actor_epic2 PRIVATE sta_vm)
add_test(NAME actor_epic2 COMMAND test_actor_epic2)

# ── Phase 2 Epic 3: Mailbox & Message Send ──────────────────────────────
add_executable(test_mailbox test_mailbox.c)
target_include_directories(test_mailbox PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_mailbox PRIVATE sta_vm)
add_test(NAME mailbox COMMAND test_mailbox)

add_executable(test_epic3_integration test_epic3_integration.c)
target_include_directories(test_epic3_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_epic3_integration PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_epic3_integration PRIVATE sta_vm)
add_test(NAME epic3_integration COMMAND test_epic3_integration)

add_executable(test_actor_dispatch test_actor_dispatch.c)
target_include_directories(test_actor_dispatch PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_actor_dispatch PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_actor_dispatch PRIVATE sta_vm)
add_test(NAME actor_dispatch COMMAND test_actor_dispatch)

add_executable(test_actor_send test_actor_send.c)
target_include_directories(test_actor_send PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_actor_send PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_actor_send PRIVATE sta_vm)
add_test(NAME actor_send COMMAND test_actor_send)

add_executable(test_mailbox_heap_growth test_mailbox_heap_growth.c)
target_include_directories(test_mailbox_heap_growth PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_mailbox_heap_growth PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_mailbox_heap_growth PRIVATE sta_vm)
add_test(NAME mailbox_heap_growth COMMAND test_mailbox_heap_growth)

add_executable(test_ask_mailbox_full test_ask_mailbox_full.c)
target_include_directories(test_ask_mailbox_full PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_ask_mailbox_full PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_ask_mailbox_full PRIVATE sta_vm)
add_test(NAME ask_mailbox_full COMMAND test_ask_mailbox_full)

add_executable(test_deep_copy test_deep_copy.c)
target_include_directories(test_deep_copy PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_deep_copy PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_deep_copy PRIVATE sta_vm)
add_test(NAME deep_copy COMMAND test_deep_copy)

# ── Fix #295: Deep Copy Heap Growth ──────────────────────────────────────
add_executable(test_deep_copy_growth test_deep_copy_growth.c)
target_include_directories(test_deep_copy_growth PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_deep_copy_growth PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_deep_copy_growth PRIVATE sta_vm)
add_test(NAME deep_copy_growth COMMAND test_deep_copy_growth)

# ── Phase 2 Epic 4: Scheduler ──────────────────────────────────────────
find_package(Threads REQUIRED)

add_executable(test_scheduler test_scheduler.c)
target_include_directories(test_scheduler PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_scheduler PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_scheduler PRIVATE sta_vm Threads::Threads)
add_test(NAME scheduler COMMAND test_scheduler)

add_executable(test_scheduler_auto test_scheduler_auto.c)
target_include_directories(test_scheduler_auto PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_scheduler_auto PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_scheduler_auto PRIVATE sta_vm Threads::Threads)
add_test(NAME scheduler_auto COMMAND test_scheduler_auto)

add_executable(test_preemption test_preemption.c)
target_include_directories(test_preemption PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_preemption PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_preemption PRIVATE sta_vm Threads::Threads)
add_test(NAME preemption COMMAND test_preemption)

add_executable(test_scheduler_mt test_scheduler_mt.c)
target_include_directories(test_scheduler_mt PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_scheduler_mt PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_scheduler_mt PRIVATE sta_vm Threads::Threads)
add_test(NAME scheduler_mt COMMAND test_scheduler_mt)

add_executable(test_work_stealing test_work_stealing.c)
target_include_directories(test_work_stealing PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_work_stealing PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_work_stealing PRIVATE sta_vm Threads::Threads)
add_test(NAME work_stealing COMMAND test_work_stealing)

add_executable(test_eval_integration test_eval_integration.c)
target_include_directories(test_eval_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_eval_integration PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_eval_integration PRIVATE sta_vm Threads::Threads)
add_test(NAME eval_integration COMMAND test_eval_integration)

add_executable(test_scheduler_stress test_scheduler_stress.c)
target_include_directories(test_scheduler_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_scheduler_stress PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_scheduler_stress PRIVATE sta_vm Threads::Threads)
add_test(NAME scheduler_stress COMMAND test_scheduler_stress)
set_tests_properties(scheduler_stress PROPERTIES TIMEOUT 30)

# ── Phase 2 Epic 5: Per-Actor GC ──────────────────────────────────────────
add_executable(test_gc test_gc.c)
target_include_directories(test_gc PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_gc PRIVATE sta_vm)
add_test(NAME gc COMMAND test_gc)

add_executable(test_gc_heap test_gc_heap.c)
target_include_directories(test_gc_heap PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_gc_heap PRIVATE sta_vm)
add_test(NAME gc_heap COMMAND test_gc_heap)

add_executable(test_gc_roots test_gc_roots.c)
target_include_directories(test_gc_roots PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_gc_roots PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_gc_roots PRIVATE sta_vm)
add_test(NAME gc_roots COMMAND test_gc_roots)

add_executable(test_gc_safety test_gc_safety.c)
target_include_directories(test_gc_safety PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_gc_safety PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_gc_safety PRIVATE sta_vm)
add_test(NAME gc_safety COMMAND test_gc_safety)

add_executable(test_gc_stats test_gc_stats.c)
target_include_directories(test_gc_stats PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_gc_stats PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_gc_stats PRIVATE sta_vm)
add_test(NAME gc_stats COMMAND test_gc_stats)

add_executable(test_gc_scheduled test_gc_scheduled.c)
target_include_directories(test_gc_scheduled PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_gc_scheduled PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_gc_scheduled PRIVATE sta_vm Threads::Threads)
add_test(NAME gc_scheduled COMMAND test_gc_scheduled)
set_tests_properties(gc_scheduled PROPERTIES TIMEOUT 60)

# ── Phase 2 Epic 6: Supervision ──────────────────────────────────────────
add_executable(test_supervisor_linkage test_supervisor_linkage.c)
target_include_directories(test_supervisor_linkage PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_supervisor_linkage PRIVATE sta_vm)
add_test(NAME supervisor_linkage COMMAND test_supervisor_linkage)

add_executable(test_failure_detection test_failure_detection.c)
target_include_directories(test_failure_detection PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_failure_detection PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_failure_detection PRIVATE sta_vm Threads::Threads)
add_test(NAME failure_detection COMMAND test_failure_detection)

add_executable(test_restart test_restart.c)
target_include_directories(test_restart PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_restart PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_restart PRIVATE sta_vm Threads::Threads)
add_test(NAME restart COMMAND test_restart)

add_executable(test_stop_escalate test_stop_escalate.c)
target_include_directories(test_stop_escalate PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_stop_escalate PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_stop_escalate PRIVATE sta_vm Threads::Threads)
add_test(NAME stop_escalate COMMAND test_stop_escalate)

add_executable(test_restart_intensity test_restart_intensity.c)
target_include_directories(test_restart_intensity PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_restart_intensity PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_restart_intensity PRIVATE sta_vm Threads::Threads)
add_test(NAME restart_intensity COMMAND test_restart_intensity)

add_executable(test_supervision_tree test_supervision_tree.c)
target_include_directories(test_supervision_tree PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_supervision_tree PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_supervision_tree PRIVATE sta_vm Threads::Threads)
add_test(NAME supervision_tree COMMAND test_supervision_tree)

add_executable(test_supervision_stress test_supervision_stress.c)
target_include_directories(test_supervision_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_supervision_stress PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_supervision_stress PRIVATE sta_vm Threads::Threads)
add_test(NAME supervision_stress COMMAND test_supervision_stress)
set_tests_properties(supervision_stress PROPERTIES TIMEOUT 120)

# ── Actor Registry (Cleanup Story 1) ─────────────────────────────────
add_executable(test_actor_registry test_actor_registry.c)
target_include_directories(test_actor_registry PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_actor_registry PRIVATE sta_vm)
add_test(NAME actor_registry COMMAND test_actor_registry)

# ── Actor Reference Counting (fixes #316, #317) ────────────────────────
add_executable(test_actor_refcount test_actor_refcount.c)
target_include_directories(test_actor_refcount PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_actor_refcount PRIVATE sta_vm Threads::Threads)
add_test(NAME actor_refcount COMMAND test_actor_refcount)

# ── Phase 2 Epic 7A: Futures Infrastructure ──────────────────────────────
add_executable(test_future test_future.c)
target_include_directories(test_future PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_future PRIVATE sta_vm Threads::Threads)
add_test(NAME future COMMAND test_future)

add_executable(test_ask_send test_ask_send.c)
target_include_directories(test_ask_send PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_ask_send PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_ask_send PRIVATE sta_vm)
add_test(NAME ask_send COMMAND test_ask_send)

add_executable(test_reply_routing test_reply_routing.c)
target_include_directories(test_reply_routing PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_reply_routing PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_reply_routing PRIVATE sta_vm Threads::Threads)
add_test(NAME reply_routing COMMAND test_reply_routing)

# ── Phase 2 Epic 7B: Future wait, crash safety, stress ───────────────────
add_executable(test_future_wait test_future_wait.c)
target_include_directories(test_future_wait PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_future_wait PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_future_wait PRIVATE sta_vm Threads::Threads)
add_test(NAME future_wait COMMAND test_future_wait)

add_executable(test_future_crash test_future_crash.c)
target_include_directories(test_future_crash PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_future_crash PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_future_crash PRIVATE sta_vm Threads::Threads)
add_test(NAME future_crash COMMAND test_future_crash)
set_tests_properties(future_crash PROPERTIES TIMEOUT 30)

add_executable(test_future_integration test_future_integration.c)
target_include_directories(test_future_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_future_integration PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_future_integration PRIVATE sta_vm Threads::Threads)
add_test(NAME future_integration COMMAND test_future_integration)
set_tests_properties(future_integration PROPERTIES TIMEOUT 60)

# ── Future soak test (Epic 7A + 7B — now enabled) ────────────────────────
add_executable(test_future_soak test_future_soak.c)
target_include_directories(test_future_soak PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_future_soak PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_future_soak PRIVATE sta_vm Threads::Threads)
add_test(NAME future_soak COMMAND test_future_soak)
set_tests_properties(future_soak PROPERTIES TIMEOUT 60)

# ── Soak test (manual, not in default ctest) ─────────────────────────────
add_executable(test_soak test_soak.c)
target_include_directories(test_soak PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_soak PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_soak PRIVATE sta_vm Threads::Threads)
add_test(NAME soak COMMAND test_soak)
set_tests_properties(soak PROPERTIES LABELS "soak" TIMEOUT 300 DISABLED TRUE)

# ── Microbenchmark (manual, not in default ctest) ────────────────────────
add_executable(test_microbench test_microbench.c)
target_include_directories(test_microbench PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_microbench PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel")
target_link_libraries(test_microbench PRIVATE sta_vm)
add_test(NAME microbench COMMAND test_microbench)
set_tests_properties(microbench PROPERTIES LABELS "soak" TIMEOUT 60 DISABLED TRUE)

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

# ── Spike 003: Work-stealing scheduler — superseded by production tests ────
# Build target retained for reference; ctest target removed (was DISABLED).
add_executable(test_scheduler_spike
    test_scheduler_spike.c
    ../src/scheduler/scheduler_spike.c)
target_include_directories(test_scheduler_spike PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_scheduler_spike PRIVATE -fsanitize=thread)
target_link_options(test_scheduler_spike PRIVATE -fsanitize=thread)
target_link_libraries(test_scheduler_spike PRIVATE Threads::Threads)

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

# ── Spike 006: Image save/load ────────────────────────────────────────────
add_executable(test_image_spike
    test_image_spike.c
    ../src/image/image_spike.c)
target_include_directories(test_image_spike PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_image_spike PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fsanitize=thread)
target_link_options(test_image_spike PRIVATE -fsanitize=thread)
add_test(NAME image_spike COMMAND test_image_spike)
set_tests_properties(image_spike PROPERTIES TIMEOUT 60)

# Benchmark build — no TSan, -O2, run manually for throughput numbers
add_executable(bench_image_spike
    test_image_spike.c
    ../src/image/image_spike.c)
target_include_directories(bench_image_spike PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(bench_image_spike PRIVATE STA_BENCH=1)
target_compile_options(bench_image_spike PRIVATE -O2 -Wall -Wextra)

# ── Spike 007: Native bridge — TSan correctness gate ─────────────────────
add_executable(test_bridge_spike
    test_bridge_spike.c
    ../src/bridge/bridge_spike.c)
target_include_directories(test_bridge_spike PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(test_bridge_spike PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fsanitize=thread -g)
target_link_options(test_bridge_spike PRIVATE -fsanitize=thread)
target_link_libraries(test_bridge_spike PRIVATE Threads::Threads)
add_test(NAME bridge_spike COMMAND test_bridge_spike)
set_tests_properties(bridge_spike PROPERTIES TIMEOUT 60)

# ── Spike 003: Per-deque-variant benchmark build ───────────────────────────
add_executable(bench_scheduler_deque_b
    test_scheduler_spike.c
    ../src/scheduler/scheduler_spike.c)
target_include_directories(bench_scheduler_deque_b PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(bench_scheduler_deque_b
    PRIVATE STA_BENCH=1 STA_DEQUE_VARIANT=2)
target_compile_options(bench_scheduler_deque_b PRIVATE -O2)
target_link_libraries(bench_scheduler_deque_b PRIVATE Threads::Threads)
