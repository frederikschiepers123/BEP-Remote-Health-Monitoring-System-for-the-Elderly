# Standalone hardware bring-up programs (CLAUDE.md §15).
# Each is an independent .uf2 you flash to validate one layer of the stack.
# Enabled with -DBUILD_BRINGUP=ON; not part of the normal firmware build.

# tusb_config.h now lives in components/transport_usb (not the repo root), so it
# no longer shadows pico_stdio_usb's own tusb_config.h. These programs can safely
# inherit the global include path (lwipopts.h / mbedtls_config.h) from the root.

# Step 1 — bare-metal blink (no FreeRTOS). Validates toolchain + flash + boot.
add_executable(bringup_blink blink.c)
target_link_libraries(bringup_blink PRIVATE
    pico_stdlib
    pico_cyw43_arch_none
)
pico_add_extra_outputs(bringup_blink)

# Step 2 — FreeRTOS hello (one task blinks). Validates the SMP port + config.
add_executable(bringup_freertos freertos_hello.c)
target_link_libraries(bringup_freertos PRIVATE
    pico_stdlib
    pico_cyw43_arch_none
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
pico_add_extra_outputs(bringup_freertos)

# Step 3 — USB-serial dev console. Validates the PC log path (pico_stdio_usb).
# This is a developer console, NOT the deferred USB-CDC tablet link.
add_executable(bringup_usb_console usb_console.c)
target_link_libraries(bringup_usb_console PRIVATE
    pico_stdlib
    pico_stdio_usb
    pico_cyw43_arch_none
)
pico_enable_stdio_usb(bringup_usb_console 1)
pico_enable_stdio_uart(bringup_usb_console 0)
pico_add_extra_outputs(bringup_usb_console)

# Step 4 — littlefs mount + boot counter. Exercises the real storage component
# and logs over the USB console. board provides err.h (storage links it PRIVATE).
add_executable(bringup_storage storage_counter.c)
target_link_libraries(bringup_storage PRIVATE
    storage
    board
    pico_stdlib
    pico_stdio_usb
    pico_cyw43_arch_none
)
pico_enable_stdio_usb(bringup_storage 1)
pico_enable_stdio_uart(bringup_storage 0)
pico_add_extra_outputs(bringup_storage)

# Factory provisioning — writes UUID + ECDSA cert chain + per-device config
# into the §11 canonical paths over USB-serial. Replaces compile-time-baked
# certs. Driven by scripts/provision_device.py. Watchdog-reboots on REBOOT.
add_executable(bringup_provision provision.c)
target_link_libraries(bringup_provision PRIVATE
    storage
    board
    pico_stdlib
    pico_stdio_usb
    pico_cyw43_arch_none
    pico_mbedtls          # SHA-256 over each artefact
    hardware_watchdog
)
pico_enable_stdio_usb(bringup_provision 1)
pico_enable_stdio_uart(bringup_provision 0)
pico_add_extra_outputs(bringup_provision)

# BME280 driver verification — no Wi-Fi, no MQTT. Inits I²C0, scans the bus,
# initialises the BME280 and prints samples to PuTTY every second. Cleanest
# way to confirm sensor wiring + driver before involving the network stack.
add_executable(bringup_bme280
    bme280_only.c
    ${CMAKE_SOURCE_DIR}/components/sensor_env/bme280.c
)
target_include_directories(bringup_bme280 PRIVATE
    ${CMAKE_SOURCE_DIR}/components/sensor_env   # bme280.h
)
target_link_libraries(bringup_bme280 PRIVATE
    board
    log
    pico_stdlib
    pico_stdio_usb
    pico_cyw43_arch_none    # CYW43 must be initialised for any stdio_usb path
    hardware_i2c
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
pico_enable_stdio_usb(bringup_bme280 1)
pico_enable_stdio_uart(bringup_bme280 0)
pico_add_extra_outputs(bringup_bme280)

# ENS160 driver verification — same shape as bringup_bme280.
add_executable(bringup_ens160
    ens160_only.c
    ${CMAKE_SOURCE_DIR}/components/sensor_air/ens160.c
)
target_include_directories(bringup_ens160 PRIVATE
    ${CMAKE_SOURCE_DIR}/components/sensor_air
)
target_link_libraries(bringup_ens160 PRIVATE
    board
    log
    pico_stdlib
    pico_stdio_usb
    pico_cyw43_arch_none
    hardware_i2c
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
pico_enable_stdio_usb(bringup_ens160 1)
pico_enable_stdio_uart(bringup_ens160 0)
pico_add_extra_outputs(bringup_ens160)

# MR60BHA2 radar driver verification — UART on GP4/GP5.
# Links the existing components/sensor_radar/radar_bha2.c directly (avoids
# the sensor_radar library's radar_task / radar_select dependencies which
# pull in FreeRTOS queues and storage for /cfg lookup).
add_executable(bringup_radar
    radar_only.c
    ${CMAKE_SOURCE_DIR}/components/sensor_radar/radar_bha2.c
)
target_include_directories(bringup_radar PRIVATE
    ${CMAKE_SOURCE_DIR}/components/sensor_radar
)
target_link_libraries(bringup_radar PRIVATE
    board
    log
    pico_stdlib
    pico_stdio_usb
    pico_cyw43_arch_none
    hardware_uart
    hardware_gpio
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
pico_enable_stdio_usb(bringup_radar 1)
pico_enable_stdio_uart(bringup_radar 0)
pico_add_extra_outputs(bringup_radar)

# SH1106 OLED status pages with live BME280 + ENS160 readings.
add_executable(bringup_oled
    oled_only.c
    ${CMAKE_SOURCE_DIR}/components/ui_oled/sh1122.c
    ${CMAKE_SOURCE_DIR}/components/sensor_env/bme280.c
    ${CMAKE_SOURCE_DIR}/components/sensor_air/ens160.c
)
target_include_directories(bringup_oled PRIVATE
    ${CMAKE_SOURCE_DIR}/components/ui_oled
    ${CMAKE_SOURCE_DIR}/components/sensor_env
    ${CMAKE_SOURCE_DIR}/components/sensor_air
)
target_link_libraries(bringup_oled PRIVATE
    board
    log
    pico_stdlib
    pico_stdio_usb
    pico_cyw43_arch_none
    hardware_i2c
    hardware_gpio
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
pico_enable_stdio_usb(bringup_oled 1)
pico_enable_stdio_uart(bringup_oled 0)
pico_add_extra_outputs(bringup_oled)

# Step 5 — Wi-Fi associate + DHCP, with /cfg provisioning. Mirrors the real
# firmware's FreeRTOS + lwIP stack. Credentials come from env vars or -D cache
# vars at configure time (NEVER committed); they are baked into this dev build
# only. build/ is gitignored.
add_executable(bringup_wifi wifi_connect.c)
# Shadow the root lwipopts.h with a bring-up-specific one (NO_SYS=1 for the
# threadsafe_background arch). The BEFORE keyword ensures this path is searched
# before the global include_directories(${CMAKE_SOURCE_DIR}) added at the top
# of the root CMakeLists.txt — otherwise we'd pick up the firmware's NO_SYS=0
# lwipopts.h which needs sys_arch.h (only present under sys_freertos).
target_include_directories(bringup_wifi BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/lwip   # bringup-only lwipopts.h
)
target_link_libraries(bringup_wifi PRIVATE
    storage
    board
    pico_stdlib
    pico_stdio_usb
    # NOTE: We deliberately use the threadsafe_background arch for this bring-up,
    # NOT pico_cyw43_arch_lwip_sys_freertos (which is what the real firmware
    # targets). On this SDK + FreeRTOS-Kernel rev, cyw43_arch_init under
    # sys_freertos hangs waiting on its internal worker task even with a clean
    # FreeRTOSConfig. threadsafe_background polls cyw43 from an alarm-pool
    # background callback instead — same lwIP, no FreeRTOS dependency in the
    # network init path — and is enough to prove creds + chip + DHCP. The real
    # firmware will revisit sys_freertos with the right async_context config.
    pico_cyw43_arch_lwip_threadsafe_background
    pico_mbedtls   # lwipopts.h enables LWIP_ALTCP_TLS_MBEDTLS -> lwip needs mbedtls headers
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
# Pull credentials from -D<var> or environment; quote into string literals.
foreach(_v WIFI_SSID WIFI_PSK BROKER_HOST BROKER_IP BROKER_PORT)
    if(NOT DEFINED ${_v} AND DEFINED ENV{${_v}})
        set(${_v} "$ENV{${_v}}")
    endif()
endforeach()
# Wrap each define as ONE quoted CMake argument so values containing spaces
# (e.g. SSIDs like "Huize Smith") survive the shell -> CMake -> compiler path.
if(DEFINED WIFI_SSID)
    target_compile_definitions(bringup_wifi PRIVATE "WIFI_SSID=\"${WIFI_SSID}\"")
endif()
if(DEFINED WIFI_PSK)
    target_compile_definitions(bringup_wifi PRIVATE "WIFI_PSK=\"${WIFI_PSK}\"")
endif()
if(DEFINED BROKER_HOST)
    target_compile_definitions(bringup_wifi PRIVATE "BROKER_HOST=\"${BROKER_HOST}\"")
endif()
if(DEFINED BROKER_IP)
    target_compile_definitions(bringup_wifi PRIVATE "BROKER_IP=\"${BROKER_IP}\"")
endif()
if(DEFINED BROKER_PORT)
    target_compile_definitions(bringup_wifi PRIVATE "BROKER_PORT=${BROKER_PORT}")
endif()
pico_enable_stdio_usb(bringup_wifi 1)
pico_enable_stdio_uart(bringup_wifi 0)
pico_add_extra_outputs(bringup_wifi)

# Step 6 — mTLS to broker + MQTT CONNECT + test publish.
# Skips the over-USB cert upload flow (deferred) and bakes the device cert/key
# + CA + UUID into the UF2 from a provisioning bundle. The bundle is produced
# by scripts/provision_ca.sh; point CERTS_DIR at out/device-<uuid>/. Same
# security stance as the WiFi creds: env-var → build → gitignored.
if(NOT DEFINED CERTS_DIR AND DEFINED ENV{CERTS_DIR})
    set(CERTS_DIR "$ENV{CERTS_DIR}")
endif()
if(DEFINED CERTS_DIR)
    # Resolve CERTS_DIR to an absolute path. Without this, a relative value
    # (e.g. `out/device-<uuid>`) gets interpreted by add_custom_command's
    # DEPENDS relative to the current binary dir (`build/test/bringup/`),
    # so the build looks for `test/bringup/out/...` and fails.
    get_filename_component(CERTS_DIR_ABS "${CERTS_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    set(_baked_h ${CMAKE_CURRENT_BINARY_DIR}/baked_certs.h)
    add_custom_command(
        OUTPUT  ${_baked_h}
        COMMAND ${CMAKE_COMMAND} -E env python3
                ${CMAKE_SOURCE_DIR}/scripts/bake_certs.py
                ${CERTS_DIR_ABS} ${_baked_h}
        DEPENDS ${CERTS_DIR_ABS}/ca.der
                ${CERTS_DIR_ABS}/dev.crt
                ${CERTS_DIR_ABS}/dev.key
                ${CERTS_DIR_ABS}/device.json
                ${CMAKE_SOURCE_DIR}/scripts/bake_certs.py
        COMMENT "Baking device certs from ${CERTS_DIR_ABS} into baked_certs.h"
        VERBATIM
    )
    add_executable(bringup_mqtt mqtt_connect.c ${_baked_h})
    target_include_directories(bringup_mqtt BEFORE PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lwip   # bringup-only lwipopts.h (NO_SYS=1)
        ${CMAKE_CURRENT_BINARY_DIR}        # baked_certs.h
    )
    target_link_libraries(bringup_mqtt PRIVATE
        pico_stdlib
        pico_stdio_usb
        pico_cyw43_arch_lwip_threadsafe_background
        pico_lwip_mqtt          # lwIP's built-in MQTT client (raw API; NO_SYS=1)
        pico_lwip_mbedtls       # altcp_tls glue between lwIP and mbedTLS
        pico_mbedtls            # mbedTLS proper
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap4
    )
    foreach(_v WIFI_SSID WIFI_PSK BROKER_IP BROKER_PORT)
        if(NOT DEFINED ${_v} AND DEFINED ENV{${_v}})
            set(${_v} "$ENV{${_v}}")
        endif()
    endforeach()
    if(DEFINED WIFI_SSID)
        target_compile_definitions(bringup_mqtt PRIVATE "WIFI_SSID=\"${WIFI_SSID}\"")
    endif()
    if(DEFINED WIFI_PSK)
        target_compile_definitions(bringup_mqtt PRIVATE "WIFI_PSK=\"${WIFI_PSK}\"")
    endif()
    if(DEFINED BROKER_IP)
        target_compile_definitions(bringup_mqtt PRIVATE "BROKER_IP=\"${BROKER_IP}\"")
    endif()
    if(DEFINED BROKER_PORT)
        target_compile_definitions(bringup_mqtt PRIVATE "BROKER_PORT=${BROKER_PORT}")
    endif()
    pico_enable_stdio_usb(bringup_mqtt 1)
    pico_enable_stdio_uart(bringup_mqtt 0)
    pico_add_extra_outputs(bringup_mqtt)

    # ── Sensor publish bring-up — extends mqtt_connect with real sensor reads ──
    # Same TLS+MQTT path as bringup_mqtt; adds I²C bus init + BME280 sampling
    # + per-sensor topic publish (rmms/<uuid>/env now; /air and /radar coming).
    # bme280.c is pulled in directly to avoid the sensor_env wrapper's FreeRTOS
    # queue / app_config dependencies (those belong to firmware-proper, M8).
    add_executable(bringup_sensors
        sensors_publish.c
        ${CMAKE_SOURCE_DIR}/components/sensor_env/bme280.c
        ${CMAKE_SOURCE_DIR}/components/sensor_air/ens160.c
        ${CMAKE_SOURCE_DIR}/components/ui_oled/sh1122.c
        ${_baked_h}
    )
    target_include_directories(bringup_sensors BEFORE PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lwip
        ${CMAKE_CURRENT_BINARY_DIR}
    )
    target_include_directories(bringup_sensors PRIVATE
        ${CMAKE_SOURCE_DIR}/components/sensor_env   # bme280.h
        ${CMAKE_SOURCE_DIR}/components/sensor_air   # ens160.h
        ${CMAKE_SOURCE_DIR}/components/ui_oled      # sh1122.h
    )
    target_link_libraries(bringup_sensors PRIVATE
        board
        log
        pico_stdlib
        pico_stdio_usb
        hardware_i2c
        pico_cyw43_arch_lwip_threadsafe_background
        pico_lwip_mqtt
        pico_lwip_mbedtls
        pico_mbedtls
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap4
    )
    if(DEFINED WIFI_SSID)
        target_compile_definitions(bringup_sensors PRIVATE "WIFI_SSID=\"${WIFI_SSID}\"")
    endif()
    if(DEFINED WIFI_PSK)
        target_compile_definitions(bringup_sensors PRIVATE "WIFI_PSK=\"${WIFI_PSK}\"")
    endif()
    if(DEFINED BROKER_IP)
        target_compile_definitions(bringup_sensors PRIVATE "BROKER_IP=\"${BROKER_IP}\"")
    endif()
    if(DEFINED BROKER_PORT)
        target_compile_definitions(bringup_sensors PRIVATE "BROKER_PORT=${BROKER_PORT}")
    endif()
    pico_enable_stdio_usb(bringup_sensors 1)
    pico_enable_stdio_uart(bringup_sensors 0)
    pico_add_extra_outputs(bringup_sensors)
else()
    message(STATUS "bringup_mqtt + bringup_sensors: skipped (set CERTS_DIR=out/device-<uuid> to build)")
endif()
