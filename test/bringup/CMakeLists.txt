# Standalone hardware bring-up programs (CLAUDE.md §15).
# Each is an independent .uf2 you flash to validate one layer of the stack.
# Enabled with -DBUILD_BRINGUP=ON; not part of the normal firmware build.

# There is no custom tusb_config.h in the tree (the USB-CDC stack was removed —
# ADR-0002), so pico_stdio_usb uses its own. These programs safely inherit the
# global include path (lwipopts.h / mbedtls_config.h) from the repo root.

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

# BMP280 driver verification — no Wi-Fi, no MQTT. Inits I²C0, scans the bus,
# initialises the BMP280 and prints samples to PuTTY every second. Cleanest
# way to confirm sensor wiring + driver before involving the network stack.
add_executable(bringup_bmp280
    bmp280_only.c
    ${CMAKE_SOURCE_DIR}/components/sensor_env/bmp280.c
)
target_include_directories(bringup_bmp280 PRIVATE
    ${CMAKE_SOURCE_DIR}/components/sensor_env   # bmp280.h
)
target_link_libraries(bringup_bmp280 PRIVATE
    board
    log
    pico_stdlib
    pico_stdio_usb
    pico_cyw43_arch_none    # CYW43 must be initialised for any stdio_usb path
    hardware_i2c
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
pico_enable_stdio_usb(bringup_bmp280 1)
pico_enable_stdio_uart(bringup_bmp280 0)
pico_add_extra_outputs(bringup_bmp280)

# ENS160 driver verification — same shape as bringup_bmp280.
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

# AHT21-only I²C diagnostic — continuous timeout-bounded re-scan + read, no
# network. Fastest way to isolate "does the AHT21 answer the bus" from PCB
# pull-up/power/wiring problems. Runs at 100 kHz (see aht21_only.c).
add_executable(bringup_aht21
    aht21_only.c
    ${CMAKE_SOURCE_DIR}/components/sensor_env/aht21.c
)
target_include_directories(bringup_aht21 PRIVATE
    ${CMAKE_SOURCE_DIR}/components/sensor_env
)
target_link_libraries(bringup_aht21 PRIVATE
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
pico_enable_stdio_usb(bringup_aht21 1)
pico_enable_stdio_uart(bringup_aht21 0)
pico_add_extra_outputs(bringup_aht21)

# AHT21 + ENS160 combined I²C diagnostic — continuous timeout-bounded re-scan +
# read of both combo-board sensors (AHT21 temp/hum compensates the ENS160), no
# network. Runs at 100 kHz (see aht21_ens160_only.c).
add_executable(bringup_aht21_ens160
    aht21_ens160_only.c
    ${CMAKE_SOURCE_DIR}/components/sensor_env/aht21.c
    ${CMAKE_SOURCE_DIR}/components/sensor_air/ens160.c
)
target_include_directories(bringup_aht21_ens160 PRIVATE
    ${CMAKE_SOURCE_DIR}/components/sensor_env
    ${CMAKE_SOURCE_DIR}/components/sensor_air
)
target_link_libraries(bringup_aht21_ens160 PRIVATE
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
pico_enable_stdio_usb(bringup_aht21_ens160 1)
pico_enable_stdio_uart(bringup_aht21_ens160 0)
pico_add_extra_outputs(bringup_aht21_ens160)

# ENS160 decisive diagnostic — raw-I²C clone/power/driver discriminator: reads
# the app FW version (GPR path) + polls DEVICE_STATUS after a single STANDARD
# write with NO OPMODE re-kicking. No driver, no network. See ens160_diag.c.
add_executable(bringup_ens160_diag ens160_diag.c)
target_link_libraries(bringup_ens160_diag PRIVATE
    board
    pico_stdlib
    pico_stdio_usb
    pico_cyw43_arch_none
    hardware_i2c
    hardware_gpio
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
pico_enable_stdio_usb(bringup_ens160_diag 1)
pico_enable_stdio_uart(bringup_ens160_diag 0)
pico_add_extra_outputs(bringup_ens160_diag)

# All-sensors BENCH tool — AHT21 + ENS160 + BH1750 (I²C) + MR60BHA2 radar
# (UART) printed continuously with NO network. Verifies the full sensor set
# without a broker/hotspot, unlike bringup_sensors which only prints I²C reads
# after MQTT CONNACK. See sensors_bench.c.
add_executable(bringup_bench
    sensors_bench.c
    ${CMAKE_SOURCE_DIR}/components/sensor_env/aht21.c
    ${CMAKE_SOURCE_DIR}/components/sensor_air/ens160.c
    ${CMAKE_SOURCE_DIR}/components/sensor_light/bh1750.c
    ${CMAKE_SOURCE_DIR}/components/sensor_radar/radar_bha2.c
)
target_include_directories(bringup_bench PRIVATE
    ${CMAKE_SOURCE_DIR}/components/sensor_env
    ${CMAKE_SOURCE_DIR}/components/sensor_air
    ${CMAKE_SOURCE_DIR}/components/sensor_light
    ${CMAKE_SOURCE_DIR}/components/sensor_radar
)
target_link_libraries(bringup_bench PRIVATE
    board
    log
    pico_stdlib
    pico_stdio_usb
    pico_cyw43_arch_none
    hardware_i2c
    hardware_uart
    hardware_gpio
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
pico_enable_stdio_usb(bringup_bench 1)
pico_enable_stdio_uart(bringup_bench 0)
pico_add_extra_outputs(bringup_bench)

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

# SH1122 OLED status pages with live BMP280 + ENS160 readings.
add_executable(bringup_oled
    oled_only.c
    ${CMAKE_SOURCE_DIR}/components/ui_oled/sh1122.c
    ${CMAKE_SOURCE_DIR}/components/sensor_env/bmp280.c
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
# Identity (UUID + cert chain) and config (Wi-Fi, broker) live in littlefs,
# provisioned with bringup_provision + scripts/provision_device.py. Reflashing
# either UF2 below leaves /certs and /cfg untouched.
add_executable(bringup_mqtt mqtt_connect.c)
target_include_directories(bringup_mqtt BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/lwip   # bringup-only lwipopts.h (NO_SYS=1)
)
target_link_libraries(bringup_mqtt PRIVATE
    board
    log
    storage
    identity
    cfg
    pico_stdlib
    pico_stdio_usb
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mqtt          # lwIP's built-in MQTT client (raw API; NO_SYS=1)
    pico_lwip_mbedtls       # altcp_tls glue between lwIP and mbedTLS
    pico_mbedtls            # mbedTLS proper
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
pico_enable_stdio_usb(bringup_mqtt 1)
pico_enable_stdio_uart(bringup_mqtt 0)
pico_add_extra_outputs(bringup_mqtt)

# ── Sensor publish bring-up — extends mqtt_connect with real sensor reads ──
# Same TLS+MQTT path as bringup_mqtt; adds I²C bus init + BMP280 + ENS160
# sampling + per-sensor topic publish (rmms/<uuid>/env, /air; /radar NYI).
# Driver .c files are pulled in directly to avoid the per-sensor wrapper
# libraries' FreeRTOS queue / app_config dependencies (those belong to
# firmware-proper, M8).
add_executable(bringup_sensors
    sensors_publish.c
    ${CMAKE_SOURCE_DIR}/components/sensor_air/ens160.c
    ${CMAKE_SOURCE_DIR}/components/sensor_radar/radar_bha2.c
    ${CMAKE_SOURCE_DIR}/components/sensor_radar/radar_filter.c
    ${CMAKE_SOURCE_DIR}/components/ui_oled/sh1122.c
)
target_include_directories(bringup_sensors BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/lwip
)
target_include_directories(bringup_sensors PRIVATE
    ${CMAKE_SOURCE_DIR}/components/sensor_air    # ens160.h
    ${CMAKE_SOURCE_DIR}/components/sensor_radar  # radar_driver.h
    ${CMAKE_SOURCE_DIR}/components/ui_oled       # sh1122.h
)
target_link_libraries(bringup_sensors PRIVATE
    board
    log
    storage
    identity
    cfg
    sensor_env        # env_driver_t + BMP280/AHT21 + env_select
    sensor_light      # light_driver_t + BH1750/GL5516 + light_select
    pico_stdlib
    pico_stdio_usb
    hardware_i2c
    hardware_uart
    hardware_gpio
    hardware_adc
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mqtt
    pico_lwip_mbedtls
    pico_mbedtls
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
pico_enable_stdio_usb(bringup_sensors 1)
pico_enable_stdio_uart(bringup_sensors 0)
pico_add_extra_outputs(bringup_sensors)

# Diagnostic clone — same as bringup_sensors but I²C clocked at 100 kHz.
# Use when the bus has marginal pull-ups (e.g. a sensor breakout that carried
# its own 4.7 kΩ resistors got removed). Same .uf2 layout, different filename.
add_executable(bringup_sensors_slow
    sensors_publish.c
    ${CMAKE_SOURCE_DIR}/components/sensor_air/ens160.c
    ${CMAKE_SOURCE_DIR}/components/sensor_radar/radar_bha2.c
    ${CMAKE_SOURCE_DIR}/components/sensor_radar/radar_filter.c
    ${CMAKE_SOURCE_DIR}/components/ui_oled/sh1122.c
)
target_include_directories(bringup_sensors_slow BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/lwip
)
target_include_directories(bringup_sensors_slow PRIVATE
    ${CMAKE_SOURCE_DIR}/components/sensor_air
    ${CMAKE_SOURCE_DIR}/components/sensor_radar
    ${CMAKE_SOURCE_DIR}/components/ui_oled
)
target_compile_definitions(bringup_sensors_slow PRIVATE BOARD_I2C_FREQ_HZ=100000U)
target_link_libraries(bringup_sensors_slow PRIVATE
    board log storage identity cfg sensor_env sensor_light
    pico_stdlib pico_stdio_usb
    hardware_i2c hardware_uart hardware_gpio hardware_adc
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mqtt pico_lwip_mbedtls pico_mbedtls
    FreeRTOS-Kernel FreeRTOS-Kernel-Heap4
)
pico_enable_stdio_usb(bringup_sensors_slow 1)
pico_enable_stdio_uart(bringup_sensors_slow 0)
pico_add_extra_outputs(bringup_sensors_slow)
