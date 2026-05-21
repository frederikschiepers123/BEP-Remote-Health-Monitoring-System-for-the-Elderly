cmake_minimum_required(VERSION 3.22)

# Board must be pico2_w — checked early so the error is clear.
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2_w)
endif()
if(NOT PICO_BOARD STREQUAL "pico2_w")
    message(FATAL_ERROR "PICO_BOARD must be pico2_w (got '${PICO_BOARD}')")
endif()

include(pico_sdk_import.cmake)

project(sensor_module C ASM)
set(CMAKE_C_STANDARD 11)

# Strict warnings — any new warning is a build error.
add_compile_options(
    -Wall -Wextra -Werror -Wpedantic
    -Wshadow -Wconversion -Wdouble-promotion
    -Wno-unused-parameter   # remove once all stubs are filled
)

pico_sdk_init()

# ── FreeRTOS ──────────────────────────────────────────────────────────────────
# Expects FreeRTOS-Kernel as a git submodule at third_party/FreeRTOS-Kernel.
# The pico-sdk ≥ 2.1.0 ships a FreeRTOS_Kernel_import.cmake shim.
set(FREERTOS_KERNEL_PATH ${CMAKE_SOURCE_DIR}/third_party/FreeRTOS-Kernel)
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake)

# ── littlefs ──────────────────────────────────────────────────────────────────
add_subdirectory(third_party/littlefs)

# ── Components ───────────────────────────────────────────────────────────────
add_subdirectory(components/board)
add_subdirectory(components/log)
add_subdirectory(components/storage)
add_subdirectory(components/json)
add_subdirectory(components/identity)
add_subdirectory(components/transport_usb)
add_subdirectory(components/transport_wifi)
add_subdirectory(components/transport_selector)
add_subdirectory(components/tls_context)
add_subdirectory(components/mqtt_client)
add_subdirectory(components/sensor_env)
add_subdirectory(components/sensor_radar)
add_subdirectory(components/sensor_light)
add_subdirectory(components/ui_oled)
add_subdirectory(components/ui_input)

# ── Application ───────────────────────────────────────────────────────────────
add_subdirectory(main)

# ── Tests (host-native, optional) ────────────────────────────────────────────
option(BUILD_HOST_TESTS "Build host-native unit tests (requires CMocka)" OFF)
if(BUILD_HOST_TESTS)
    enable_testing()
    add_subdirectory(test/host)
endif()
