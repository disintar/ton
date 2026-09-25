add_subdirectory(consensus)

add_executable(test-external-message-relay test-external-message-relay.cpp)
target_include_directories(test-external-message-relay PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test-external-message-relay PRIVATE td-test-main tdutils)
add_test(NAME test-external-message-relay COMMAND test-external-message-relay)

if(TON_USE_TONCENTER_RELAY)
add_executable(toncenter-relay-client toncenter-relay-client.cpp)
target_link_libraries(toncenter-relay-client PRIVATE toncenter-relay)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME test-toncenter-relay-http
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-toncenter-relay-http.py $<TARGET_FILE:toncenter-relay-client>)
endif()
endif()

add_executable(test-archive-peer-selection test-archive-peer-selection.cpp)
target_include_directories(test-archive-peer-selection PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test-archive-peer-selection PRIVATE td-test-main tdutils)
add_test(NAME test-archive-peer-selection COMMAND test-archive-peer-selection)

add_executable(test-archive-sync-policy test-archive-sync-policy.cpp)
target_include_directories(test-archive-sync-policy PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test-archive-sync-policy PRIVATE td-test-main tdutils)
add_test(NAME test-archive-sync-policy COMMAND test-archive-sync-policy)

add_executable(test-download-race test-download-race.cpp)
target_include_directories(test-download-race PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test-download-race PRIVATE td-test-main tdactor tdutils)
add_test(NAME test-download-race COMMAND test-download-race)
