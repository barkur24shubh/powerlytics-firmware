#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARTIFACT_DIR="${ROOT_DIR}/artifacts"
APP_BUILD_DIR="${ROOT_DIR}/Debug"
BOOT_BUILD_DIR="${ROOT_DIR}/Bootloader/Debug"
OBJCOPY="${OBJCOPY:-arm-none-eabi-objcopy}"
BOARD_SLUG="${POWERLYTICS_BOARD_SLUG:-}"
BOARD_SKU="${POWERLYTICS_BOARD_SKU:-}"
FIRMWARE_VERSION="$(sed -n 's/^#define APP_FIRMWARE_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "${ROOT_DIR}/Core/Inc/app_firmware.h" | head -n 1)"

"${OBJCOPY}" -O binary \
  "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.elf" \
  "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.bin"

# Pad the app image with 0xFF (erased flash) to a multiple of 64 bytes.
# Firmware before 1.0.6 and bootloaders built before 2026-11-17 compute a
# wrong SHA-256 for inputs whose length mod 64 is 56-63, so an image of such
# a size would be refused as corrupt by every unit still running them.
python3 - "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.bin" <<'PY'
import sys
path = sys.argv[1]
with open(path, "rb") as f:
    data = f.read()
pad = (-len(data)) % 64
if pad:
    with open(path, "ab") as f:
        f.write(b"\xff" * pad)
print(f"app image {len(data)} -> {len(data) + pad} bytes (multiple of 64)")
PY

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

if [[ -n "${BOARD_SLUG}" ]]; then
  board_dir="${ARTIFACT_DIR}/boards/${BOARD_SLUG}"
  board_name="powerlytics-${BOARD_SLUG}-${FIRMWARE_VERSION:-unknown}"

  mkdir -p "${board_dir}"

  cp "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.bin" "${board_dir}/"
  cp "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.hex" "${board_dir}/"
  cp "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.elf" "${board_dir}/"
  cp "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.map" "${board_dir}/"

  cp "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.bin" "${board_dir}/${board_name}.bin"
  cp "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.hex" "${board_dir}/${board_name}.hex"
  cp "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.elf" "${board_dir}/${board_name}.elf"
  cp "${APP_BUILD_DIR}/BALCKPILL_BASED_GENERIC_GATEWAY.map" "${board_dir}/${board_name}.map"

  cp "${BOOT_BUILD_DIR}/powerlytics_bootloader.bin" "${board_dir}/powerlytics_bootloader.bin"
  cp "${BOOT_BUILD_DIR}/powerlytics_bootloader.hex" "${board_dir}/powerlytics_bootloader.hex"
  cp "${BOOT_BUILD_DIR}/powerlytics_bootloader.elf" "${board_dir}/powerlytics_bootloader.elf"
  cp "${BOOT_BUILD_DIR}/powerlytics_bootloader.map" "${board_dir}/powerlytics_bootloader.map"

  cat > "${board_dir}/manifest.json" <<EOF
{
  "type": "powerlytics_board_firmware_artifacts",
  "boardSku": "${BOARD_SKU}",
  "boardSlug": "${BOARD_SLUG}",
  "firmwareVersion": "${FIRMWARE_VERSION:-unknown}",
  "appBinary": "${board_name}.bin",
  "legacyAppBinary": "BALCKPILL_BASED_GENERIC_GATEWAY.bin",
  "bootloaderBinary": "powerlytics_bootloader.bin"
}
EOF

  (
    cd "${board_dir}"
    sha256sum *.bin *.hex *.elf *.map manifest.json > SHA256SUMS
  )
fi

echo "Packaged current build outputs into ${ARTIFACT_DIR}"
