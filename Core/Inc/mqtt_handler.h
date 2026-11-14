#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include "stm32f4xx_hal.h"
#include "w25q16.h"
#include <stdint.h>

/* Public API */
uint8_t MQTT_Connect(const char *host, int port, const char *user, const char *pass);
uint8_t MQTT_Subscribe(const char *topic);
uint8_t MQTT_Publish(const char *topic, const char *payload);
uint8_t MQTT_PublishPriority(const char *topic, const char *payload);
char* MQTT_Subscriber_Run(void);
void    handle_downlink_cjson(const char *payload);
void    Prepare_MQTT_Topics(void);
uint8_t MQTT_DrainQueuedStatus(uint8_t max_msgs);
uint8_t MQTT_HasPendingConfig(void);
uint8_t MQTT_ApplyPendingConfig_AndReset(void);
uint8_t mqttPublish(const char *message);
uint8_t MQTT_CheckDisconnectURC(void);
uint8_t MQTT_Maintain(uint32_t keepalive_ms,
                      const char *topic,
                      const char *ping_payload,
                      uint8_t *attempted);
uint8_t MQTT_LastPublishAttempted(void);
const char *MQTT_LastPublishStage(void);
void    MQTT_SetOtaFlash(W25Q_Handle *flash);
uint8_t MQTT_OtaIsBusy(void);
uint8_t MQTT_ReportOtaBootResultOnce(void);

#endif
