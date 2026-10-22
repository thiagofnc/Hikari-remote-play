# Controller mapping + Controller Settings window.
file(GLOB HIKARI_CLIENT_UI_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/input_mapping.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/controller_settings*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/joycon_*.cpp)
if(HIKARI_CLIENT_UI_SOURCES)
    add_library(hikari_client_ui STATIC ${HIKARI_CLIENT_UI_SOURCES})
    target_include_directories(hikari_client_ui PUBLIC src)
    target_link_libraries(hikari_client_ui PUBLIC hikari_shared d2d1 dwrite d3d11 dxgi windowscodecs user32 gdi32 comctl32 xinput)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/ui_demo/CMakeLists.txt)
    add_subdirectory(ui_demo)
endif()
