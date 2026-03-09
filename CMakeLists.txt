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
    src/actor/actor.c
)

target_include_directories(sta_vm
    PUBLIC  include
    PRIVATE src
)

target_compile_options(sta_vm PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -Wno-unused-parameter
)

option(STA_USE_LIBUV "Enable libuv async I/O substrate (Phase 2)" OFF)
if(STA_USE_LIBUV)
    find_package(libuv REQUIRED)
    target_link_libraries(sta_vm PRIVATE libuv::libuv)
    target_compile_definitions(sta_vm PRIVATE STA_USE_LIBUV=1)
endif()

install(TARGETS sta_vm ARCHIVE DESTINATION lib)
install(FILES include/sta/vm.h DESTINATION include/sta)

enable_testing()
add_subdirectory(tests)
add_subdirectory(examples/embed_basic)
