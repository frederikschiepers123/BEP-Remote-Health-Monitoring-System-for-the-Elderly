add_library(mqtt_client STATIC mqtt_client.c)

target_include_directories(mqtt_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(mqtt_client
    PUBLIC
        board
        log
    PRIVATE
        FreeRTOS-Kernel
        tls_context
        json
        pico_stdlib
)
