add_library(cfg STATIC cfg.c)

target_include_directories(cfg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(cfg PRIVATE
    board
    log
    storage
    json
)
