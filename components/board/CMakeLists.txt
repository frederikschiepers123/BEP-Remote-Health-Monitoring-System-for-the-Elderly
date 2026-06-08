add_library(board INTERFACE)

target_include_directories(board INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(board INTERFACE
    pico_stdlib
    hardware_i2c
    hardware_spi
    hardware_uart
    hardware_adc
    hardware_gpio
)
