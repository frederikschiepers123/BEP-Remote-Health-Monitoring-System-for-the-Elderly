add_library(sensor_env STATIC
    sensor_env.c
    bme280.c
    aht21.c
    env_select.c
)

target_include_directories(sensor_env PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/main
)

target_link_libraries(sensor_env PUBLIC
    board
    log
    storage
    cfg
    i2c_bus
    pico_stdlib
    hardware_i2c
    FreeRTOS-Kernel
)
