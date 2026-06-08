add_library(sensor_light STATIC
    sensor_light.c
)

target_include_directories(sensor_light PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/main
)

target_link_libraries(sensor_light PUBLIC
    board
    log
    pico_stdlib
    hardware_adc
    FreeRTOS-Kernel
)
