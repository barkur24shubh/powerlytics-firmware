#pragma once
#include <stdbool.h>
#include "modbus_config.h"

typedef struct cJSON cJSON;

bool modbus_cfg_from_json(const char *json, modbus_cfg_t *cfg);
bool modbus_cfg_from_cjson(cJSON *root, modbus_cfg_t *cfg);
