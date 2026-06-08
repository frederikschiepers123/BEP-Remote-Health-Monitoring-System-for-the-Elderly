add_library(identity STATIC identity.c)

target_include_directories(identity PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(identity PRIVATE
    board
    log
    storage
)
