add_library(tls_context STATIC tls_context.c)

target_include_directories(tls_context PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tls_context
    PUBLIC
        board
        log
        identity
        pico_mbedtls_headers   # tls_context.h exposes mbedtls/ssl.h types
    PRIVATE
        storage        # tls_context.c loads cert/key via storage
        pico_mbedtls
        pico_stdlib
)
