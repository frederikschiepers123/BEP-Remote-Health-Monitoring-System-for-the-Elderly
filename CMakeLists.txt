cmake_minimum_required(VERSION 3.22)

# Board must be pico2_w — checked early so the error is clear.
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2_w)
endif()
if(NOT PICO_BOARD STREQUAL "pico2_w")
    message(FATAL_ERROR "PICO_BOARD must be pico2_w (got '${PICO_BOARD}')")
endif()

include(pico_sdk_import.cmake)

project(sensor_module C CXX ASM)
set(CMAKE_C_STANDARD 11)

# The repo root holds the pico-sdk user-config headers that SDK components
# locate by name on the include path: mbedtls_config.h, lwipopts.h,
# tusb_config.h (FreeRTOSConfig.h is handled via FREERTOS_CONFIG_FILE_DIRECTORY).
# Put the root on the global include path so every target that compiles SDK
# sources can find them.
include_directories(${CMAKE_SOURCE_DIR})

# Strict warnings — applied to FIRST-PARTY targets only (see end of file).
# They must NOT leak onto the pico-sdk, FreeRTOS, or littlefs, which carry
# intentional conversions/shadows that -Werror would (wrongly) reject.
set(FIRMWARE_WARNINGS
    -Wall -Wextra -Werror -Wpedantic
    -Wshadow -Wconversion -Wdouble-promotion
    -Wno-unused-parameter   # remove once all stubs are filled
)

pico_sdk_init()

# ── FreeRTOS ──────────────────────────────────────────────────────────────────
# Expects FreeRTOS-Kernel as a git submodule at third_party/FreeRTOS-Kernel.
# The pico-sdk ≥ 2.1.0 ships a FreeRTOS_Kernel_import.cmake shim.
# FreeRTOSConfig.h lives at the repo root; the port adds this dir to the
# FreeRTOS-Kernel interface include path.
set(FREERTOS_KERNEL_PATH ${CMAKE_SOURCE_DIR}/third_party/FreeRTOS-Kernel)
set(FREERTOS_CONFIG_FILE_DIRECTORY ${CMAKE_SOURCE_DIR})
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake)

# ── littlefs ──────────────────────────────────────────────────────────────────
# Upstream littlefs ships no CMakeLists.txt (just lfs.c / lfs_util.c), so define
# the `littlefs` target here from its sources. The storage component links it.
add_library(littlefs STATIC
    third_party/littlefs/lfs.c
    third_party/littlefs/lfs_util.c
)
target_include_directories(littlefs PUBLIC third_party/littlefs)

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

# ── Strict warnings on first-party sources only ──────────────────────────────
# The pico-sdk distributes its .c files through INTERFACE libraries, so they are
# compiled *into* each consuming target. Applying the strict set at target level
# would therefore (wrongly) impose -Werror on SDK/FreeRTOS code. Instead apply it
# per-source-file to each target's own declared sources. board is INTERFACE
# (header-only) and has nothing to compile.
foreach(_tgt
    log storage json identity
    transport_usb transport_wifi transport_selector
    tls_context mqtt_client
    sensor_env sensor_radar sensor_light
    ui_oled ui_input
    sensor_module)
    get_target_property(_srcs ${_tgt} SOURCES)
    if(_srcs)
        set_source_files_properties(${_srcs}
            TARGET_DIRECTORY ${_tgt}
            PROPERTIES COMPILE_OPTIONS "${FIRMWARE_WARNINGS}")
    endif()
endforeach()

# ── Tests (host-native, optional) ────────────────────────────────────────────
option(BUILD_HOST_TESTS "Build host-native unit tests (requires CMocka)" OFF)
if(BUILD_HOST_TESTS)
    enable_testing()
    add_subdirectory(test/host)
endif()

# ── Hardware bring-up programs (CLAUDE.md §15, optional) ──────────────────────
option(BUILD_BRINGUP "Build standalone on-target bring-up programs (§15)" OFF)
if(BUILD_BRINGUP)
    add_subdirectory(test/bringup)
endif()
