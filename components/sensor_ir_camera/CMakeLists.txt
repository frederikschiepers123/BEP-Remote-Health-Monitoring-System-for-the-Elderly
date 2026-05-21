add_library(sensor_ir_camera STATIC
    sensor_ir_camera.c
)

target_include_directories(sensor_ir_camera PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/main
)

target_link_libraries(sensor_ir_camera PUBLIC
    board
    log
    pico_stdlib
    hardware_spi
    FreeRTOS-Kernel
)
