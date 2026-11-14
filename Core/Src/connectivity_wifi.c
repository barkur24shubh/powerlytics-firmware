#include "connectivity_backend.h"

const connectivity_backend_t *connectivity_wifi_backend(void)
{
    static const connectivity_backend_t backend = {
        .name = "wifi",
    };

    return &backend;
}
