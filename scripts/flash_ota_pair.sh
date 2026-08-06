#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STM32_CLI="${STM32_CLI:-/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI}"
TOOLBIN="${TOOLBIN:-/Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.macos64_1.0.0.202411102158/tools/bin}"
STLINK_SN="${STLINK_SN:-37FF71064E5734363EE01443}"

export PATH="${TOOLBIN}:${PATH}"
export OBJCOPY="${OBJCOPY:-${TOOLBIN}/arm-none-eabi-objcopy}"

make -C "${ROOT_DIR}/Bootloader" clean all
make -C "${ROOT_DIR}/Debug" -j8 all

"${ROOT_DIR}/scripts/package_current_artifacts.sh"

"${STM32_CLI}" -c "port=SWD" "sn=${STLINK_SN}" "mode=UR" "reset=HWrst" \
  -w "${ROOT_DIR}/Bootloader/Debug/powerlytics_bootloader.elf" -v

"${STM32_CLI}" -c "port=SWD" "sn=${STLINK_SN}" "mode=UR" "reset=HWrst" \
  -w "${ROOT_DIR}/Debug/BALCKPILL_BASED_GENERIC_GATEWAY.elf" -v -rst

"${STM32_CLI}" -c "port=SWD" "sn=${STLINK_SN}" "mode=HotPlug" \
  -r32 0x08000000 8 -r32 0x08008000 8 -coreReg PC LR MSP PSP XPSR IPSR

echo "Flashed bootloader + app and refreshed artifacts."
if command -v sha256sum >/dev/null 2>&1; then
  sha256sum "${ROOT_DIR}/artifacts/app/BALCKPILL_BASED_GENERIC_GATEWAY.bin"
else
  shasum -a 256 "${ROOT_DIR}/artifacts/app/BALCKPILL_BASED_GENERIC_GATEWAY.bin"
fi
