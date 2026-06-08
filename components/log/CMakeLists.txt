add_library(log STATIC log.c)

target_include_directories(log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(log PRIVATE
    board
    pico_stdlib
)
