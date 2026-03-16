cmake_minimum_required(VERSION 3.20...4.2)
project(smalltalk-a-vm LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(sta_vm STATIC
    src/vm/vm.c
    src/vm/handle.c
    src/vm/eval.c
    src/vm/oop.c
    src/vm/immutable_space.c
    src/vm/heap.c
    src/vm/special_objects.c
    src/vm/class_table.c
    src/vm/symbol_table.c
    src/vm/method_dict.c
    src/vm/special_selectors.c
    src/vm/compiled_method.c
    src/vm/frame.c
    src/vm/selector.c
    src/vm/primitive_table.c
    src/vm/interpreter.c
    src/vm/handler.c
    src/actor/actor.c
    src/bootstrap/bootstrap.c
    src/bootstrap/filein.c
    src/bootstrap/kernel_load.c
    src/compiler/scanner.c
    src/compiler/ast.c
    src/compiler/parser.c
    src/compiler/codegen.c
    src/compiler/compiler.c
    src/image/image.c
)

target_include_directories(sta_vm
    PUBLIC  include
    PRIVATE src
)

target_compile_options(sta_vm PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -Wno-unused-parameter
)

target_compile_definitions(sta_vm PRIVATE
    KERNEL_DIR="${CMAKE_SOURCE_DIR}/kernel"
)

option(STA_USE_LIBUV "Enable libuv async I/O (required for Spike 005)" OFF)
if(STA_USE_LIBUV)
    # Homebrew libuv ships a CMake config; add Homebrew prefix so find_package
    # locates it without requiring PKG_CONFIG_PATH to be set.
    list(APPEND CMAKE_PREFIX_PATH /opt/homebrew)
    find_package(libuv CONFIG REQUIRED)
    # Targets: libuv::uv (shared) and libuv::uv_a (static).
    set(LIBUV_TARGET libuv::uv)
endif()

install(TARGETS sta_vm ARCHIVE DESTINATION lib)
install(FILES include/sta/vm.h DESTINATION include/sta)

enable_testing()
add_subdirectory(tests)
add_subdirectory(examples/embed_basic)
