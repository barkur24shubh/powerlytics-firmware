#include "config_apply.h"
#include "modbus_config_json.h"
#include "FreeRTOS.h"
#include <stdio.h>
#include <string.h>

static struct {
    uint8_t pending;
    char configId[64];
} s_cfg;

uint8_t cfg_stage_pending(const char *cfg_min_json,
                          const char *config_id)
{
    if (!cfg_min_json || !config_id) return 0;

    modbus_cfg_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    memset(&s_cfg, 0, sizeof(s_cfg));

    if (!modbus_cfg_from_json(cfg_min_json, &parsed)) {
        return 0;
    }

    snprintf(s_cfg.configId, sizeof(s_cfg.configId), "%s", config_id);
    s_cfg.pending = 1;
    return 1;
}

uint8_t cfg_is_apply_pending(void)
{
    return s_cfg.pending;
}

const modbus_cfg_t* cfg_get_pending(void)
{
    return NULL;
}

const char* cfg_get_pending_config_id(void)
{
    return s_cfg.pending ? s_cfg.configId : NULL;
}

void cfg_clear_pending(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
}
