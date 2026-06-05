add_library(storage STATIC storage.c)

target_include_directories(storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(storage PRIVATE
    board
    log
    littlefs
    pico_stdlib
    pico_flash
    hardware_flash
    hardware_sync
)
