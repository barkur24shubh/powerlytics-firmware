#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARTIFACT_DIR="${ROOT_DIR}/artifacts"
APP_BUILD_DIR="${ROOT_DIR}/Debug"
BOOT_BUILD_DIR="${ROOT_DIR}/Bootloader/Debug"
OBJCOPY="${OBJCOPY:-arm-none-eabi-objcopy}"

"${OBJCOPY}" -O binary \
  "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.elf" \
  "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.bin"

"${OBJCOPY}" -O ihex \
  "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.elf" \
  "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.hex"

required=(
  "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.bin"
  "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.hex"
  "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.elf"
  "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.map"
  "${BOOT_BUILD_DIR}/powerlytics_bootloader.bin"
  "${BOOT_BUILD_DIR}/powerlytics_bootloader.hex"
  "${BOOT_BUILD_DIR}/powerlytics_bootloader.elf"
  "${BOOT_BUILD_DIR}/powerlytics_bootloader.map"
)

for file in "${required[@]}"; do
  if [[ ! -f "${file}" ]]; then
    echo "Missing build output: ${file}" >&2
    echo "Run ./scripts/flash_ota_pair.sh or build Bootloader and Debug first." >&2
    exit 1
  fi
done

mkdir -p "${ARTIFACT_DIR}/app" "${ARTIFACT_DIR}/bootloader"

cp "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.bin" "${ARTIFACT_DIR}/app/"
cp "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.hex" "${ARTIFACT_DIR}/app/"
cp "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.elf" "${ARTIFACT_DIR}/app/"
cp "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.map" "${ARTIFACT_DIR}/app/"

cp "${BOOT_BUILD_DIR}/powerlytics_bootloader.bin" "${ARTIFACT_DIR}/bootloader/"
cp "${BOOT_BUILD_DIR}/powerlytics_bootloader.hex" "${ARTIFACT_DIR}/bootloader/"
cp "${BOOT_BUILD_DIR}/powerlytics_bootloader.elf" "${ARTIFACT_DIR}/bootloader/"
cp "${BOOT_BUILD_DIR}/powerlytics_bootloader.map" "${ARTIFACT_DIR}/bootloader/"

cp "${ARTIFACT_DIR}/app/BALCKPILL_BASED_GENERIC_GATEWAY.bin" "${ARTIFACT_DIR}/"
cp "${ARTIFACT_DIR}/app/BALCKPILL_BASED_GENERIC_GATEWAY.hex" "${ARTIFACT_DIR}/"
cp "${ARTIFACT_DIR}/app/BALCKPILL_BASED_GENERIC_GATEWAY.elf" "${ARTIFACT_DIR}/"
cp "${ARTIFACT_DIR}/app/BALCKPILL_BASED_GENERIC_GATEWAY.map" "${ARTIFACT_DIR}/"

(
  cd "${ARTIFACT_DIR}"
  sha256sum BALCKPILL_BASED_GENERIC_GATEWAY.* > SHA256SUMS
)

echo "Packaged current build outputs into ${ARTIFACT_DIR}"
