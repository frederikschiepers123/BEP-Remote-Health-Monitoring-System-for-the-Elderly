# Standard pico-sdk import shim.
# Either set PICO_SDK_PATH in your environment/CMake cache, or place pico-sdk
# as a submodule at third_party/pico-sdk.

if(DEFINED ENV{PICO_SDK_PATH} AND (NOT PICO_SDK_PATH))
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    message("Using PICO_SDK_PATH from environment: '${PICO_SDK_PATH}'")
endif()

if(NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH ${CMAKE_SOURCE_DIR}/third_party/pico-sdk)
    message("PICO_SDK_PATH not set; trying submodule at '${PICO_SDK_PATH}'")
endif()

get_filename_component(PICO_SDK_PATH "${PICO_SDK_PATH}" REALPATH BASE_DIR "${CMAKE_BINARY_DIR}")

if(NOT EXISTS "${PICO_SDK_PATH}/pico_sdk_init.cmake")
    message(FATAL_ERROR "Cannot find pico-sdk at '${PICO_SDK_PATH}'. "
            "Set PICO_SDK_PATH or run: git submodule update --init third_party/pico-sdk")
endif()

set(PICO_SDK_INIT_CMAKE_FILE ${PICO_SDK_PATH}/pico_sdk_init.cmake)
include(${PICO_SDK_INIT_CMAKE_FILE})
