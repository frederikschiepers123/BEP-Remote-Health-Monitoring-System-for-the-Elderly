# Host-native unit tests (CLAUDE.md §14.1).
#
# This is a STANDALONE native project — it must NOT be configured by the
# top-level firmware CMake (that one runs pico_sdk_init() and cross-compiles
# for the Cortex-M33, which can't produce runnable host executables).
#
# Build + run:
#   cmake -S test/host -B build-host -DCMAKE_PREFIX_PATH=$HOME/.local
#   cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
#
# Or the zero-config path (no cmake): test/host/run.sh
#
# CMocka: apt `libcmocka-dev`, or build to ~/.local (see test/host/run.sh notes).

cmake_minimum_required(VERSION 3.22)
project(rmms_host_tests C)

set(CMAKE_C_STANDARD 11)
enable_testing()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(STUBS "${CMAKE_CURRENT_SOURCE_DIR}/stubs")

find_library(CMOCKA_LIB cmocka)
find_path(CMOCKA_INC cmocka.h)
if(NOT CMOCKA_LIB OR NOT CMOCKA_INC)
    message(FATAL_ERROR "CMocka not found. Pass -DCMAKE_PREFIX_PATH=$HOME/.local "
                        "or install libcmocka-dev.")
endif()

# add_host_test(<name> <driver-source-relative-to-repo>)
function(add_host_test name driver)
    add_executable(${name} ${name}.c ${REPO_ROOT}/${driver} ${STUBS}/host_stubs.c)
    target_compile_definitions(${name} PRIVATE HOST_TEST=1)
    target_include_directories(${name} PRIVATE
        ${STUBS}
        ${CMOCKA_INC}
        ${REPO_ROOT}/components/board
        ${REPO_ROOT}/components/log
        ${REPO_ROOT}/components/sensor_env
        ${REPO_ROOT}/components/sensor_air
        ${REPO_ROOT}/components/sensor_light
        ${REPO_ROOT}/components/sensor_radar
        ${REPO_ROOT}/components/spool
    )
    target_link_libraries(${name} PRIVATE ${CMOCKA_LIB} m)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_host_test(test_sensor_env  components/sensor_env/bme280.c)     # BME280 compensation
add_host_test(test_aht21       components/sensor_env/aht21.c)      # AHT21/AHT20 decode
add_host_test(test_bh1750      components/sensor_light/bh1750.c)   # BH1750 lux conversion
add_host_test(test_ens160      components/sensor_air/ens160.c)     # ENS160 validity + decode
add_host_test(test_radar_bha2  components/sensor_radar/radar_bha2.c) # MR60BHA2 frame parser
add_host_test(test_radar_filter components/sensor_radar/radar_filter.c) # ADR-0005 plausibility filter
add_host_test(test_spool       components/spool/spool.c)            # NV outbound FIFO (ADR-0003)
