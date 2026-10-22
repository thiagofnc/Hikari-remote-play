add_executable(hikari_tests
    test_link.cpp
    test_shared.cpp
    test_transport.cpp)
target_link_libraries(hikari_tests PRIVATE hikari_shared)
target_include_directories(hikari_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME hikari_tests COMMAND hikari_tests)
