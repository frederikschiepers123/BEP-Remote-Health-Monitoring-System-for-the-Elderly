add_library(transport_selector STATIC transport_selector.c)

target_include_directories(transport_selector PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(transport_selector
    PUBLIC
        board
        log
        identity
    PRIVATE
        FreeRTOS-Kernel
        transport_usb
        transport_wifi
        tls_context
        mqtt_client
        pico_stdlib
)
