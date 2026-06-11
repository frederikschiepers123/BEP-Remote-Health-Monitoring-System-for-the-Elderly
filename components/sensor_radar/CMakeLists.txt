add_library(sensor_radar STATIC
    radar_select.c
    radar_bha2.c
    radar_filter.c
    radar_task.c
)

target_include_directories(sensor_radar PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/main
)

target_link_libraries(sensor_radar PUBLIC
    board
    log
    storage
    pico_stdlib
    hardware_uart
    FreeRTOS-Kernel
)
