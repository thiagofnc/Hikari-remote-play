# hk_link: dependency-free C protocol shared with the ESP32 firmware.
add_library(hikari_link STATIC protocol/hk_link.c)
target_include_directories(hikari_link PUBLIC protocol)

add_library(hikari_shared STATIC
    common/config.cpp
    common/crypto.cpp
    common/log.cpp
    common/system.cpp
    common/udp.cpp
    protocol/net_protocol.cpp
    protocol/video_transport.cpp
    telemetry/histogram.cpp)

target_include_directories(hikari_shared PUBLIC
    common
    protocol
    serialization
    telemetry)

target_link_libraries(hikari_shared PUBLIC
    hikari_link ws2_32 avrt winmm bcrypt qwave shell32 ole32)
