#!/usr/bin/env bash
#
# build.sh — compile the sensor-module firmware that gets flashed onto the
#            Raspberry Pi Pico 2 WH (RP2350).
#
# Produces  build/sensor_module.uf2   (BOOTSEL drag-and-drop image)
# and       build/sensor_module.elf   (SWD / picotool / debugger image).
#
# This builds ONLY the firmware. Device certs and littlefs /cfg + /certs are
# factory-provisioned on a separate, access-controlled workstation (CLAUDE.md
# §10.2, §11) and are intentionally NOT generated, written, or committed here.
#
# Usage:
#   scripts/build.sh [options]
#
#   --release        RelWithDebInfo + -DNDEBUG=1 (ship build). Default: Debug.
#   --clean          Wipe the build dir before configuring.
#   -j, --jobs N     Parallel build jobs. Default: all cores.
#   --flash          After build, copy the .uf2 to a mounted RP2350 BOOTSEL drive.
#   --swd            After build, flash the .elf over SWD via OpenOCD (picoprobe).
#   --info           After build, dump the .uf2 metadata with picotool (if present).
#   -h, --help       Show this help.
#
# Environment:
#   PICO_SDK_PATH    If set and valid, used as the pico-sdk. Otherwise the
#                    third_party/pico-sdk submodule is initialised and used.
set -euo pipefail

# ── Locate repo root (this script lives in <root>/scripts) ────────────────────
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

# ── Defaults ──────────────────────────────────────────────────────────────────
BUILD_TYPE="Debug"
EXTRA_CMAKE_ARGS=()
JOBS="$(nproc 2>/dev/null || echo 4)"
DO_CLEAN=0
DO_FLASH=0
DO_SWD=0
DO_INFO=0

err()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; }
info() { printf '\033[36m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[32m  ✓\033[0m %s\n' "$*"; }

usage() { awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "${BASH_SOURCE[0]}"; }

# ── Parse args ────────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --release) BUILD_TYPE="RelWithDebInfo"; EXTRA_CMAKE_ARGS+=("-DNDEBUG=1"); shift ;;
        --debug)   BUILD_TYPE="Debug"; shift ;;
        --clean)   DO_CLEAN=1; shift ;;
        -j|--jobs) JOBS="${2:?--jobs needs a number}"; shift 2 ;;
        --flash)   DO_FLASH=1; shift ;;
        --swd)     DO_SWD=1; shift ;;
        --info)    DO_INFO=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) err "unknown option: $1"; usage; exit 2 ;;
    esac
done

# ── 1. Toolchain check ────────────────────────────────────────────────────────
info "Checking toolchain"
missing=0
for t in cmake arm-none-eabi-gcc git; do
    if ! command -v "$t" >/dev/null 2>&1; then err "missing required tool: $t"; missing=1; fi
done
[[ $missing -eq 0 ]] || { err "install the missing tools and retry"; exit 1; }

# Prefer Ninja when available (faster), else fall back to Unix Makefiles.
if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
else
    GENERATOR="Unix Makefiles"
fi
ok "cmake $(cmake --version | head -1 | awk '{print $3}'), $(arm-none-eabi-gcc -dumpversion) (gcc), generator: ${GENERATOR}"

# Fetch a third_party dependency into <path> if the dir is empty.
# Reads url + branch from .gitmodules and clones directly. We do NOT use
# `git submodule update` here: this repo declares the deps in .gitmodules but
# tracks no gitlink for them, and a stray MagicMirror gitlink (with no mapping)
# aborts the whole submodule command. A pinned shallow clone is reliable.
fetch_dep() {
    local path="$1"; shift
    local dest="${ROOT_DIR}/${path}"
    [[ -n "$(ls -A "${dest}" 2>/dev/null)" ]] && { ok "${path} (present)"; return 0; }
    local url branch
    url="$(git -C "${ROOT_DIR}" config -f .gitmodules --get "submodule.${path}.url")"
    branch="$(git -C "${ROOT_DIR}" config -f .gitmodules --get "submodule.${path}.branch" || true)"
    [[ -n "${url}" ]] || { err "no .gitmodules url for ${path}"; exit 1; }
    info "  cloning ${path}  (${url}${branch:+ @ ${branch}})"
    git clone --depth 1 ${branch:+--branch "${branch}"} "$@" "${url}" "${dest}"
    ok "${path}"
}

# ── 2. Resolve pico-sdk (env preferred, shallow clone fallback) ───────────────
if [[ -n "${PICO_SDK_PATH:-}" && -f "${PICO_SDK_PATH}/pico_sdk_init.cmake" ]]; then
    ok "Using PICO_SDK_PATH=${PICO_SDK_PATH}"
else
    info "PICO_SDK_PATH unset/invalid — fetching bundled pico-sdk"
    # pico-sdk needs its own nested deps (tinyusb, lwip, mbedtls, cyw43…).
    fetch_dep third_party/pico-sdk --recurse-submodules --shallow-submodules
    export PICO_SDK_PATH="${ROOT_DIR}/third_party/pico-sdk"
    ok "Using bundled SDK at ${PICO_SDK_PATH}"
fi

# ── 3. Mandatory deps (FreeRTOS-Kernel + littlefs) ────────────────────────────
# FreeRTOS-Kernel keeps the RP2350 ARM port in a nested submodule
# (portable/ThirdParty/Community-Supported-Ports) — clone must recurse.
info "Ensuring firmware dependencies are present"
fetch_dep third_party/FreeRTOS-Kernel --recurse-submodules --shallow-submodules
fetch_dep third_party/littlefs

# FreeRTOS RP2350 port shim must exist or the CMake include will fail loudly.
FREERTOS_SHIM="${ROOT_DIR}/third_party/FreeRTOS-Kernel/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake"
[[ -f "${FREERTOS_SHIM}" ]] || { err "FreeRTOS RP2350 shim not found at ${FREERTOS_SHIM}"; exit 1; }

# ── 4. Configure ──────────────────────────────────────────────────────────────
if [[ $DO_CLEAN -eq 1 && -d "${BUILD_DIR}" ]]; then
    info "Cleaning ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
fi

info "Configuring (${BUILD_TYPE})"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G "${GENERATOR}" \
    -DPICO_BOARD=pico2_w \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    "${EXTRA_CMAKE_ARGS[@]}"

# ── 5. Build ──────────────────────────────────────────────────────────────────
info "Building with ${JOBS} jobs"
cmake --build "${BUILD_DIR}" -j "${JOBS}"

# The executable is defined in main/, so CMake emits its artefacts there.
UF2="${BUILD_DIR}/main/sensor_module.uf2"
ELF="${BUILD_DIR}/main/sensor_module.elf"
[[ -f "${UF2}" ]] || { err "expected artefact not produced: ${UF2}"; exit 1; }

info "Build complete"
ok "$(du -h "${UF2}" | awk '{print $1}')  ${UF2}"
[[ -f "${ELF}" ]] && ok "$(du -h "${ELF}" | awk '{print $1}')  ${ELF}"
if command -v arm-none-eabi-size >/dev/null 2>&1 && [[ -f "${ELF}" ]]; then
    arm-none-eabi-size "${ELF}"
fi

# ── 6. Optional: picotool info ────────────────────────────────────────────────
if [[ $DO_INFO -eq 1 ]]; then
    if command -v picotool >/dev/null 2>&1; then
        info "picotool metadata"
        picotool info -a "${UF2}" || true
    else
        err "picotool not installed — skipping --info"
    fi
fi

# ── 7. Optional: flash via BOOTSEL mass-storage copy ──────────────────────────
if [[ $DO_FLASH -eq 1 ]]; then
    info "Flashing via BOOTSEL (mass-storage copy)"
    # The RP2 bootloader exposes INFO_UF2.TXT at the drive root. Scan the usual
    # Linux automount roots AND WSL's Windows drive mounts (/mnt/<letter>), since
    # under WSL the BOOTSEL volume appears as a Windows drive letter.
    MOUNT=""
    for root in "/run/media/${USER}"/* "/media/${USER}"/* /media/* /mnt/*; do
        [[ -f "${root}/INFO_UF2.TXT" ]] && { MOUNT="${root}"; break; }
    done
    if [[ -z "${MOUNT}" ]]; then
        err "no RP2350 BOOTSEL drive found (looked for INFO_UF2.TXT)."
        err "Hold BOOTSEL while plugging in the Pico so it enumerates as a drive,"
        err "then re-run with --flash. On WSL2 it appears as a Windows drive letter"
        err "(e.g. /mnt/e); if it does not, use --swd, or copy the .uf2 to the drive"
        err "from Windows Explorer manually."
        exit 1
    fi
    cp "${UF2}" "${MOUNT}/"
    sync
    ok "copied $(basename "${UF2}") → ${MOUNT}  (device reboots into the new firmware)"
fi

# ── 8. Optional: flash via SWD (OpenOCD + picoprobe/debugprobe) ───────────────
if [[ $DO_SWD -eq 1 ]]; then
    command -v openocd >/dev/null 2>&1 || { err "openocd not installed — cannot --swd"; exit 1; }
    info "Flashing ${ELF} over SWD"
    openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
        -c "adapter speed 5000" \
        -c "program ${ELF} verify reset exit"
    ok "SWD flash complete"
fi
