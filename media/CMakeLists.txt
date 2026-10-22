add_library(hikari_media STATIC media.cpp mingw_uuid_fix.cpp)
target_include_directories(hikari_media PUBLIC .)
target_link_libraries(hikari_media PUBLIC
    hikari_shared d3d11 dxgi mfplat mfreadwrite mf mfuuid strmiids propsys)
