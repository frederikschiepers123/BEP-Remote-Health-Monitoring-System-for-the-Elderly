add_executable(sensor_module app_main.c)

target_include_directories(sensor_module PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# v1 has no USB-CDC tablet link (ADR-0002).  LOG_* goes to a plain USB-serial
# dev console (§12); enable pico-sdk stdio over USB for it.
pico_enable_stdio_usb(sensor_module 1)
pico_enable_stdio_uart(sensor_module 0)

target_link_libraries(sensor_module PRIVATE
    board
    log
    storage
    spool
    json
    identity
    cfg
    i2c_bus
    sensor_env
    sensor_air
    sensor_radar
    sensor_light
    ui_oled
    ui_input
    transport_mqtt
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mqtt
    pico_lwip_mbedtls
    pico_mbedtls
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

pico_add_extra_outputs(sensor_module)
