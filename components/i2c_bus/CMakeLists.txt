add_library(i2c_bus STATIC i2c_bus.c)

target_include_directories(i2c_bus PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(i2c_bus PUBLIC
    board
    log
    pico_stdlib
    hardware_i2c
    hardware_gpio
    FreeRTOS-Kernel
)
