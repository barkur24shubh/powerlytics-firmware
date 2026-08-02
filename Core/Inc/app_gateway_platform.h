#ifndef APP_GATEWAY_PLATFORM_H
#define APP_GATEWAY_PLATFORM_H

#include "main.h"
#include "w25q16.h"
#include <stdint.h>

#define APP_GATEWAY_FAULT_NMI            1UL
#define APP_GATEWAY_FAULT_HARD           2UL
#define APP_GATEWAY_FAULT_MEMMANAGE      3UL
#define APP_GATEWAY_FAULT_BUS            4UL
#define APP_GATEWAY_FAULT_USAGE          5UL
#define APP_GATEWAY_FAULT_ERROR_HANDLER  6UL
#define APP_GATEWAY_FAULT_ASSERT         7UL
#define APP_GATEWAY_FAULT_MALLOC_FAILED  8UL
#define APP_GATEWAY_FAULT_STACK_OVERFLOW 9UL

typedef struct
{
    uint32_t code;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t dfsr;
    uint32_t afsr;
    uint32_t mmfar;
    uint32_t bfar;
    uint32_t pc;
    uint32_t lr;
    uint32_t count;
} app_gateway_fault_record_t;

void app_gateway_print_reset_reason(void);
uint32_t app_gateway_reset_csr(void);
const char *app_gateway_reset_reason_name(void);

uint8_t app_gateway_fault_peek(app_gateway_fault_record_t *out);
void app_gateway_fault_clear(void);
const char *app_gateway_fault_name(uint32_t code);
void app_gateway_fault_capture_and_reset(uint32_t *stacked_sp,
                                         uint32_t code,
                                         uint32_t exc_return)
                                         __attribute__((noreturn));

void app_gateway_flash_setup(W25Q_Handle *flash,
                             SPI_HandleTypeDef *hspi,
                             GPIO_TypeDef *cs_port,
                             uint16_t cs_pin);

void app_gateway_flash_test(W25Q_Handle *flash);

void app_gateway_basepri_recover(void);

#endif
