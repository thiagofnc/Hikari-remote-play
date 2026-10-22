add_library(hikari_ui STATIC ui_kit.cpp)
target_include_directories(hikari_ui PUBLIC .)
target_link_libraries(hikari_ui PUBLIC d2d1 dwrite windowscodecs ole32)
