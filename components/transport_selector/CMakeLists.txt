add_library(transport_selector STATIC transport_selector.c)

target_include_directories(transport_selector PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(transport_selector
    PUBLIC
        board
        log
        identity
        tls_context   # transport_selector.h exposes stream_t from tls_context.h
    PRIVATE
        FreeRTOS-Kernel
        transport_usb
        transport_wifi
        mqtt_client
        tinyusb_device   # transport_selector.c probes CDC via tusb.h
        pico_stdlib
)
