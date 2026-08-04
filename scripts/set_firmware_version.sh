#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 <firmware-version> [build-label]" >&2
  exit 2
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HEADER="${ROOT_DIR}/Core/Inc/app_firmware.h"
VERSION="$1"
BUILD="${2:-manual}"

tmp="$(mktemp)"
awk -v version="${VERSION}" -v build="${BUILD}" '
  /^#define APP_FIRMWARE_VERSION[[:space:]]+/ {
    print "#define APP_FIRMWARE_VERSION       \"" version "\""
    next
  }
  /^#define APP_FIRMWARE_BUILD[[:space:]]+/ {
    print "#define APP_FIRMWARE_BUILD         \"" build "\""
    next
  }
  { print }
' "${HEADER}" > "${tmp}"

mv "${tmp}" "${HEADER}"

echo "Updated ${HEADER}"
echo "APP_FIRMWARE_VERSION=${VERSION}"
echo "APP_FIRMWARE_BUILD=${BUILD}"
