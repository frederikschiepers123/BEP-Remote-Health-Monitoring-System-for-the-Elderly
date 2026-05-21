add_library(tls_context STATIC tls_context.c)

target_include_directories(tls_context PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tls_context
    PUBLIC
        board
        log
        identity
    PRIVATE
        pico_mbedtls
        pico_stdlib
)
