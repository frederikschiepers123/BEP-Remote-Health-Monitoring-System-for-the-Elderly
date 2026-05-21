add_executable(sensor_module app_main.c)

target_include_directories(sensor_module PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}   # tusb_config.h
)

# Disable pico-sdk's built-in stdio over USB/UART — TinyUSB manages CDC directly.
pico_enable_stdio_usb(sensor_module 0)
pico_enable_stdio_uart(sensor_module 0)

target_link_libraries(sensor_module PRIVATE
    board
    log
    storage
    json
    identity
    transport_usb
    transport_wifi
    transport_selector
    tls_context
    mqtt_client
    sensor_env
    sensor_radar
    sensor_light
    ui_oled
    ui_input
    pico_stdlib
    pico_cyw43_arch_lwip_sys_freertos
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

pico_add_extra_outputs(sensor_module)
