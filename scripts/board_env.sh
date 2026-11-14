#!/usr/bin/env bash

board_arg="${1:-b1-beta-c}"
board_slug="$(printf '%s' "${board_arg}" | tr '[:upper:]_' '[:lower:]-')"

case "${board_slug}" in
  b1-beta-c|beta|current)
    POWERLYTICS_BOARD_SLUG="b1-beta-c"
    POWERLYTICS_BOARD_MACRO="POWERLYTICS_BOARD_B1_BETA_C"
    POWERLYTICS_BOARD_SKU="B1-BETA-C"
    ;;
  b1-v1-c)
    POWERLYTICS_BOARD_SLUG="b1-v1-c"
    POWERLYTICS_BOARD_MACRO="POWERLYTICS_BOARD_B1_V1_C"
    POWERLYTICS_BOARD_SKU="B1-V1-C"
    ;;
  b1-v2-c)
    POWERLYTICS_BOARD_SLUG="b1-v2-c"
    POWERLYTICS_BOARD_MACRO="POWERLYTICS_BOARD_B1_V2_C"
    POWERLYTICS_BOARD_SKU="B1-V2-C"
    ;;
  b1-v3-c)
    POWERLYTICS_BOARD_SLUG="b1-v3-c"
    POWERLYTICS_BOARD_MACRO="POWERLYTICS_BOARD_B1_V3_C"
    POWERLYTICS_BOARD_SKU="B1-V3-C"
    ;;
  b1-v4-c)
    POWERLYTICS_BOARD_SLUG="b1-v4-c"
    POWERLYTICS_BOARD_MACRO="POWERLYTICS_BOARD_B1_V4_C"
    POWERLYTICS_BOARD_SKU="B1-V4-C"
    ;;
  b1-v1-e)
    POWERLYTICS_BOARD_SLUG="b1-v1-e"
    POWERLYTICS_BOARD_MACRO="POWERLYTICS_BOARD_B1_V1_E"
    POWERLYTICS_BOARD_SKU="B1-V1-E"
    ;;
  b1-v2-e)
    POWERLYTICS_BOARD_SLUG="b1-v2-e"
    POWERLYTICS_BOARD_MACRO="POWERLYTICS_BOARD_B1_V2_E"
    POWERLYTICS_BOARD_SKU="B1-V2-E"
    ;;
  b1-v3-e)
    POWERLYTICS_BOARD_SLUG="b1-v3-e"
    POWERLYTICS_BOARD_MACRO="POWERLYTICS_BOARD_B1_V3_E"
    POWERLYTICS_BOARD_SKU="B1-V3-E"
    ;;
  b1-v4-e)
    POWERLYTICS_BOARD_SLUG="b1-v4-e"
    POWERLYTICS_BOARD_MACRO="POWERLYTICS_BOARD_B1_V4_E"
    POWERLYTICS_BOARD_SKU="B1-V4-E"
    ;;
  *)
    echo "Unsupported board: ${board_arg}" >&2
    echo "Supported now: b1-beta-c" >&2
    echo "Registered future SKUs: b1-v1-c b1-v2-c b1-v3-c b1-v4-c b1-v1-e b1-v2-e b1-v3-e b1-v4-e" >&2
    return 2 2>/dev/null || exit 2
    ;;
esac

export POWERLYTICS_BOARD_SLUG
export POWERLYTICS_BOARD_MACRO
export POWERLYTICS_BOARD_SKU
