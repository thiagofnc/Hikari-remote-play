idf_component_register(
    SRCS
        "main.c"
        "host_link.c"
        "pad_store.c"
        "console_usb.c"
        "profile_hori.c"
        "profile_procon.c"
        "wake.c"
        "telemetry.c"
        "board.c"
        "ble_wake.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/protocol/hk_link.c"
    INCLUDE_DIRS
        "."
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/protocol"
    REQUIRES
        driver esp_timer nvs_flash bt
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
# Enumeration trace in console_usb.c sees every DCD event before TinyUSB does.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=dcd_event_handler")
