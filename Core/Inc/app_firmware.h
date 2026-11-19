#ifndef APP_FIRMWARE_H
#define APP_FIRMWARE_H

#include "board_profile.h"

/*
 * Device/firmware identity used by telemetry, health, logs and OTA targeting.
 * Update these values intentionally for each production release.
 */
#define APP_FIRMWARE_VERSION       "1.0.18"
#define APP_FIRMWARE_BUILD "2026-11-18-events"
#define APP_DEVICE_MODEL           BOARD_VARIANT_NAME
#define APP_OTA_SCHEMA_VERSION     1U

#endif
