# Standalone Controller Settings demo: polls XInput and opens the window, so
# the UI can be run and tested without the rest of the client.
# `hikari_controller_demo --selftest` runs the mapping unit checks.
if(TARGET hikari_client_ui)
    enable_language(RC)
    add_executable(hikari_controller_demo main.cpp ../../assets/joycon.rc)
    set_source_files_properties(../../assets/joycon.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/joycon-reference.png")
    target_link_libraries(hikari_controller_demo PRIVATE hikari_client_ui)
endif()
