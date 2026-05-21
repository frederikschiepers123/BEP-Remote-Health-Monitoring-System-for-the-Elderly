find_package(CMocka QUIET)
if(NOT CMocka_FOUND)
    message(WARNING "CMocka not found — host tests skipped")
    return()
endif()

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

target_link_libraries(test_sensor_env PRIVATE cmocka)

add_test(NAME test_sensor_env COMMAND test_sensor_env)
