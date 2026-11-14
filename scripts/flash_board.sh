#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOARD_ARG="${1:-b1-beta-c}"
STM32_CLI="${STM32_CLI:-/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI}"
STLINK_SN="${STLINK_SN:-37FF71064E5734363EE01443}"

source "${ROOT_DIR}/scripts/board_env.sh" "${BOARD_ARG}"

"${ROOT_DIR}/scripts/build_board.sh" "${POWERLYTICS_BOARD_SLUG}"

"${STM32_CLI}" -c "port=SWD" "sn=${STLINK_SN}" "mode=UR" "reset=HWrst" \
  -w "${ROOT_DIR}/Bootloader/Debug/powerlytics_bootloader.elf" -v

"${STM32_CLI}" -c "port=SWD" "sn=${STLINK_SN}" "mode=UR" "reset=HWrst" \
  -w "${ROOT_DIR}/Debug/BALCKPILL_BASED_GENERIC_GATEWAY.elf" -v -rst

"${STM32_CLI}" -c "port=SWD" "sn=${STLINK_SN}" "mode=HotPlug" \
  -r32 0x08000000 8 -r32 0x08008000 8 -coreReg PC LR MSP PSP XPSR IPSR

echo "Flashed ${POWERLYTICS_BOARD_SKU} bootloader + app and refreshed artifacts."
if command -v sha256sum >/dev/null 2>&1; then
  sha256sum "${ROOT_DIR}/artifacts/boards/${POWERLYTICS_BOARD_SLUG}/BALCKPILL_BASED_GENERIC_GATEWAY.bin"
else
  shasum -a 256 "${ROOT_DIR}/artifacts/boards/${POWERLYTICS_BOARD_SLUG}/BALCKPILL_BASED_GENERIC_GATEWAY.bin"
fi
