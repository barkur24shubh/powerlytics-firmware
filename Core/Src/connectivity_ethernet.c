#include "connectivity_backend.h"

const connectivity_backend_t *connectivity_ethernet_backend(void)
{
    static const connectivity_backend_t backend = {
        .name = "ethernet",
    };

    return &backend;
}
