#ifndef CONFIG_H
#define CONFIG_H

/*
 * Keep production secrets out of Git.
 *
 * For a local production build, create:
 *   Core/Inc/config.local.h
 *
 * Use Core/Inc/config.example.h as the template. config.local.h is ignored by
 * Git, so the device still gets its MQTT credentials at compile time without
 * publishing those credentials to GitHub.
 */
#if defined(__has_include)
#  if __has_include("config.local.h")
#    include "config.local.h"
#    define POWERLYTICS_CONFIG_LOCAL_INCLUDED 1
#  endif
#endif

#ifndef POWERLYTICS_CONFIG_LOCAL_INCLUDED
// --- Safe placeholder Network/MQTT configuration ---
#define APN                  "www"
#define MQTT_HOST_URI        "tcp://example.invalid"
#define MQTT_SNI_HOST        "example.invalid"
#define MQTT_PORT            8883
#define MQTT_USERNAME        ""
#define MQTT_PASSWORD        ""
#define MQTT_TIMEOUT_MS      10000U
#endif

// Keepalive/publish tick
#define MQTT_KEEPALIVE_SECONDS       60U
#define MQTT_KEEPALIVE_INTERVAL_MS   240000U
#define MQTT_RECONNECT_ATTEMPTS      3
#define MQTT_RECONNECT_DELAY_MS      5000U

// UART RX scratch buffer size for AT responses
#define MODEM_RX_BUF_SIZE    512



#endif
