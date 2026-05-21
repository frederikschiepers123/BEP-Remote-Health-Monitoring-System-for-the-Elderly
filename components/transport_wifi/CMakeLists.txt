add_library(transport_wifi STATIC
    transport_wifi.c
    stream_tcp.c
)

target_include_directories(transport_wifi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(transport_wifi
    PUBLIC
        board
        log
    PRIVATE
        FreeRTOS-Kernel
        pico_cyw43_arch_lwip_sys_freertos
        pico_stdlib
)
