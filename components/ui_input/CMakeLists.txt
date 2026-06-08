add_library(ui_input STATIC
    ui_input.c
)

target_include_directories(ui_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(ui_input PUBLIC
    board
    log
    pico_stdlib
    hardware_gpio
    FreeRTOS-Kernel
)
