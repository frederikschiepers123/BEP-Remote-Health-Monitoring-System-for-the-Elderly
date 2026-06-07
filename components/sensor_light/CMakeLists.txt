add_library(sensor_light STATIC
    sensor_light.c
    bh1750.c
    gl5516.c
    light_select.c
)

target_include_directories(sensor_light PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/main
)

target_link_libraries(sensor_light PUBLIC
    board
    log
    cfg
    pico_stdlib
    hardware_adc
    hardware_gpio
    hardware_i2c
    FreeRTOS-Kernel
)
