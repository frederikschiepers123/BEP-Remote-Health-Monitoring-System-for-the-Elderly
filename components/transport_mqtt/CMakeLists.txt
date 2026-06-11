add_library(transport_mqtt STATIC transport_mqtt.c)

target_include_directories(transport_mqtt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/main
)

target_link_libraries(transport_mqtt PUBLIC
    board
    log
    identity
    cfg
    json
    spool            # NV outbound FIFO (ADR-0003)
    storage          # seq persistence (/state/last_seq.json)
    ui_oled
    sensor_env       # q_env / EnvMsg
    sensor_air       # q_air / AirMsg
    sensor_radar     # q_radar / RadarSample
    sensor_light     # q_light / LightMsg
    pico_stdlib
    hardware_gpio
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mqtt
    pico_lwip_mbedtls
    pico_mbedtls
    FreeRTOS-Kernel
)
