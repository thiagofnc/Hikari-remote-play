add_executable(hikari_tests
    test_link.cpp
    test_shared.cpp
    test_transport.cpp)
target_link_libraries(hikari_tests PRIVATE hikari_shared)
target_include_directories(hikari_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME hikari_tests COMMAND hikari_tests)

add_executable(hikari_session_tests test_session.cpp)
target_link_libraries(hikari_session_tests PRIVATE hikari_client hikari_host)
add_test(NAME hikari_session_tests COMMAND hikari_session_tests)
set_tests_properties(hikari_session_tests PROPERTIES TIMEOUT 30)
add_test(NAME hikari_controller_mapping COMMAND hikari_controller_demo --selftest)
add_test(NAME hikari_app_startup COMMAND Hikari --selftest)
set_tests_properties(hikari_app_startup PROPERTIES TIMEOUT 15)

add_executable(hikari_diagram_tests test_diagram.cpp ../assets/joycon.rc)
set_source_files_properties(../assets/joycon.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../assets/joycon-reference.png;${CMAKE_CURRENT_SOURCE_DIR}/../assets/joycon2-left.png;${CMAKE_CURRENT_SOURCE_DIR}/../assets/joycon2-right.png;${CMAKE_CURRENT_SOURCE_DIR}/../assets/joycon2-left-top.png;${CMAKE_CURRENT_SOURCE_DIR}/../assets/joycon2-right-top.png;${CMAKE_CURRENT_SOURCE_DIR}/../assets/joycon2-shoulder-base.png")
target_link_libraries(hikari_diagram_tests PRIVATE hikari_client_ui)
add_test(NAME hikari_controller_diagram COMMAND hikari_diagram_tests)
