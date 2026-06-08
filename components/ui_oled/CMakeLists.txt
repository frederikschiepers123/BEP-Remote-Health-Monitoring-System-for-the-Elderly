add_library(ui_oled STATIC
    sh1122.c
    ui_oled.c
)

target_include_directories(ui_oled PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/main
)

target_link_libraries(ui_oled PUBLIC
    board
    log
    i2c_bus
    identity
    pico_stdlib
    hardware_i2c
    FreeRTOS-Kernel
)
