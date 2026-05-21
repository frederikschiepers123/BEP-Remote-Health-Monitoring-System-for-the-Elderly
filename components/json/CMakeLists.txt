add_library(json STATIC json_encode.c json_parse.c)

target_include_directories(json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(json PRIVATE board log pico_stdlib)
