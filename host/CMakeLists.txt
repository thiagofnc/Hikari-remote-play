# Host media/control building blocks (also used by the probe tools).
add_library(hikari_host_core STATIC
    src/audio_capture.cpp
    src/bridge.cpp
    src/capture.cpp
    src/encoder.cpp
    src/gpu_convert.cpp)
target_include_directories(hikari_host_core PUBLIC src)
target_link_libraries(hikari_host_core PUBLIC hikari_media setupapi cfgmgr32 ole32 oleaut32 uuid)

# The full host (session, orchestration, ABR) used by Hikari.exe.
add_library(hikari_host STATIC
    src/host_app.cpp
    src/host_net.cpp
    src/host_system.cpp)
target_link_libraries(hikari_host PUBLIC hikari_host_core iphlpapi oleaut32 advapi32 shell32)

if(HIKARI_WITH_OPUS)
    target_link_libraries(hikari_host_core PUBLIC opus)
    target_compile_definitions(hikari_host_core PUBLIC HIKARI_WITH_OPUS)
endif()
