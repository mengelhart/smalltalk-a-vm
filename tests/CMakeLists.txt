enable_testing()

add_executable(test_public_api test_public_api.c)
target_link_libraries(test_public_api PRIVATE sta_vm)

add_test(NAME test_public_api COMMAND test_public_api)

add_executable(test_objheader_spike test_objheader_spike.c)
target_include_directories(test_objheader_spike PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME objheader_spike COMMAND test_objheader_spike)
