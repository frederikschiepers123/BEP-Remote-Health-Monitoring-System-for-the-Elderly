add_library(transport_usb STATIC stream_cdc.c)

target_include_directories(transport_usb
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}   # tusb_config.h lives here (used by TinyUSB)
)

target_link_libraries(transport_usb
    PUBLIC
        board
        log
        tls_context   # stream_cdc.h exposes stream_t from tls_context.h
    PRIVATE
        FreeRTOS-Kernel
        tinyusb_device
        pico_stdlib
)
