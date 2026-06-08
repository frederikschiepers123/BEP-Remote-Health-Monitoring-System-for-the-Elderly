add_library(sensor_air STATIC
    ens160.c
    air_task.c
)

target_include_directories(sensor_air PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/main
)

target_link_libraries(sensor_air PUBLIC
    board
    log
    i2c_bus
    sensor_env       # env_last_reading() for ENS160 compensation (§3.2)
    pico_stdlib
    hardware_i2c
    FreeRTOS-Kernel
)
