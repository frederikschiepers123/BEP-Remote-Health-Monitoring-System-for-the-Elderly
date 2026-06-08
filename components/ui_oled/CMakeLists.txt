add_library(ui_oled STATIC
    sh1106.c
    ui_oled.c
)

target_include_directories(ui_oled PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/main
)

target_link_libraries(ui_oled PUBLIC
    board
    log
    transport_selector
    sensor_env
    sensor_radar
    sensor_light
    pico_stdlib
    hardware_i2c
    FreeRTOS-Kernel
)
