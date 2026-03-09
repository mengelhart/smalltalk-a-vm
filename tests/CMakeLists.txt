enable_testing()

add_executable(test_public_api test_public_api.c)
target_link_libraries(test_public_api PRIVATE sta_vm)

add_test(NAME test_public_api COMMAND test_public_api)
