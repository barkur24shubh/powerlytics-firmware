#ifndef CONFIG_LOCAL_H
#define CONFIG_LOCAL_H

// Copy this file to Core/Inc/config.local.h and fill the real values locally.
// Do not commit config.local.h.

#define APN                  "www"
#define MQTT_HOST_URI        "tcp://your-mqtt-host.example.com"
#define MQTT_SNI_HOST        "your-mqtt-host.example.com"
#define MQTT_PORT            8883
#define MQTT_USERNAME        "your-mqtt-username"
#define MQTT_PASSWORD        "your-mqtt-password"
#define MQTT_TIMEOUT_MS      10000U

#endif
