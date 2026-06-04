add_library(sensor_air STATIC ens160.c)

target_include_directories(sensor_air PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sensor_air PUBLIC
    board
    log
    pico_stdlib
    hardware_i2c
    FreeRTOS-Kernel
)
