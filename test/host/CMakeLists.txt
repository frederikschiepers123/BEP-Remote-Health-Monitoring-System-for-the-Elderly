find_package(CMocka QUIET)
if(NOT CMocka_FOUND)
    message(WARNING "CMocka not found — host tests skipped (install libcmocka-dev to run)")
    return()
endif()

# BME280 compensation formulas (datasheet reference vectors).
add_executable(test_sensor_env
    test_sensor_env.c
    ${CMAKE_SOURCE_DIR}/components/sensor_env/bme280.c
)
target_include_directories(test_sensor_env PRIVATE
    ${CMAKE_SOURCE_DIR}/components/board
    ${CMAKE_SOURCE_DIR}/components/log
    ${CMAKE_SOURCE_DIR}/components/sensor_env
)
target_compile_definitions(test_sensor_env PRIVATE HOST_TEST=1)
target_link_libraries(test_sensor_env PRIVATE cmocka m)
add_test(NAME test_sensor_env COMMAND test_sensor_env)

# AHT21 — CRC8 + 20-bit raw → temp/humidity decode.
add_executable(test_aht21
    test_aht21.c
    ${CMAKE_SOURCE_DIR}/components/sensor_env/aht21.c
)
target_include_directories(test_aht21 PRIVATE
    ${CMAKE_SOURCE_DIR}/components/board
    ${CMAKE_SOURCE_DIR}/components/log
    ${CMAKE_SOURCE_DIR}/components/sensor_env
)
target_compile_definitions(test_aht21 PRIVATE HOST_TEST=1)
target_link_libraries(test_aht21 PRIVATE cmocka m)
add_test(NAME test_aht21 COMMAND test_aht21)

# BH1750 — raw_u16 / 1.2 → lux conversion.
add_executable(test_bh1750
    test_bh1750.c
    ${CMAKE_SOURCE_DIR}/components/sensor_light/bh1750.c
)
target_include_directories(test_bh1750 PRIVATE
    ${CMAKE_SOURCE_DIR}/components/board
    ${CMAKE_SOURCE_DIR}/components/log
    ${CMAKE_SOURCE_DIR}/components/sensor_light
)
target_compile_definitions(test_bh1750 PRIVATE HOST_TEST=1)
target_link_libraries(test_bh1750 PRIVATE cmocka m)
add_test(NAME test_bh1750 COMMAND test_bh1750)
