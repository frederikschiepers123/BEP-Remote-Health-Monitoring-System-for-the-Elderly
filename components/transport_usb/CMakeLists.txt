add_library(transport_usb STATIC stream_cdc.c)

target_include_directories(transport_usb
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_SOURCE_DIR}   # tusb_config.h lives at repo root
)

target_link_libraries(transport_usb
    PUBLIC
        board
        log
    PRIVATE
        FreeRTOS-Kernel
        tinyusb_device
        pico_stdlib
)
