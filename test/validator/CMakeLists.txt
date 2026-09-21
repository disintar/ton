add_subdirectory(consensus)

add_executable(test-external-message-relay test-external-message-relay.cpp)
target_include_directories(test-external-message-relay PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test-external-message-relay PRIVATE td-test-main tdutils)
add_test(NAME test-external-message-relay COMMAND test-external-message-relay)
