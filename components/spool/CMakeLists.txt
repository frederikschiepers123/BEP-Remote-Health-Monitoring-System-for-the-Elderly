add_library(spool STATIC spool.c)

target_include_directories(spool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# PUBLIC: spool.h exposes the sensor sample structs (Env/Ens160/Radar/Light),
# so consumers of spool.h need those component headers on their include path.
target_link_libraries(spool PUBLIC
    board
    sensor_env
    sensor_air
    sensor_radar
    sensor_light
)

target_link_libraries(spool PRIVATE
    log
    pico_stdlib
    pico_flash
    hardware_flash
)
