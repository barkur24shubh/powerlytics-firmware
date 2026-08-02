#include "app_gateway_platform.h"
#include "debug_uart.h"

#define APP_GATEWAY_FAULT_MAGIC 0xFA17C0DEUL

static uint32_t s_reset_csr = 0U;

static const char *reset_reason_from_csr(uint32_t csr)
{
    if (csr & RCC_CSR_IWDGRSTF) return "IWDG";
    if (csr & RCC_CSR_WWDGRSTF) return "WWDG";
    if (csr & RCC_CSR_SFTRSTF)  return "SOFTWARE";
    if (csr & RCC_CSR_BORRSTF)  return "BROWNOUT";
    if (csr & RCC_CSR_PORRSTF)  return "POWER_ON";
    if (csr & RCC_CSR_PINRSTF)  return "PIN_RESET";
    if (csr & RCC_CSR_LPWRRSTF) return "LOW_POWER";
    return "UNKNOWN";
}

static void fault_backup_access_enable(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    PWR->CR |= PWR_CR_DBP;
    __DSB();
    __HAL_RCC_RTC_ENABLE();
    __DSB();
}

static uint8_t fault_stack_pointer_ok(const uint32_t *sp)
{
    uintptr_t addr = (uintptr_t)sp;

    if ((addr & 0x3UL) != 0UL) {
        return 0U;
    }

    if (addr >= 0x20000000UL && addr <= (0x20020000UL - 32UL)) {
        return 1U;
    }

    if (addr >= 0x10000000UL && addr <= (0x10010000UL - 32UL)) {
        return 1U;
    }

    return 0U;
}

const char *app_gateway_fault_name(uint32_t code)
{
    switch (code)
    {
        case APP_GATEWAY_FAULT_NMI:           return "NMI";
        case APP_GATEWAY_FAULT_HARD:          return "HARDFAULT";
        case APP_GATEWAY_FAULT_MEMMANAGE:     return "MEMMANAGE";
        case APP_GATEWAY_FAULT_BUS:           return "BUSFAULT";
        case APP_GATEWAY_FAULT_USAGE:         return "USAGEFAULT";
        case APP_GATEWAY_FAULT_ERROR_HANDLER: return "ERROR_HANDLER";
        case APP_GATEWAY_FAULT_ASSERT:        return "ASSERT";
        case APP_GATEWAY_FAULT_MALLOC_FAILED: return "MALLOC_FAILED";
        case APP_GATEWAY_FAULT_STACK_OVERFLOW:return "STACK_OVERFLOW";
        default:                              return "UNKNOWN";
    }
}

uint8_t app_gateway_fault_peek(app_gateway_fault_record_t *out)
{
    if (!out) {
        return 0U;
    }

    fault_backup_access_enable();

    if (RTC->BKP3R != APP_GATEWAY_FAULT_MAGIC) {
        return 0U;
    }

    out->code  = RTC->BKP4R;
    out->cfsr  = RTC->BKP5R;
    out->hfsr  = RTC->BKP6R;
    out->dfsr  = RTC->BKP7R;
    out->afsr  = RTC->BKP8R;
    out->mmfar = RTC->BKP9R;
    out->bfar  = RTC->BKP10R;
    out->pc    = RTC->BKP11R;
    out->lr    = RTC->BKP12R;
    out->count = RTC->BKP13R;

    return 1U;
}

void app_gateway_fault_clear(void)
{
    fault_backup_access_enable();

    RTC->BKP3R = 0U;
    RTC->BKP4R = 0U;
    RTC->BKP5R = 0U;
    RTC->BKP6R = 0U;
    RTC->BKP7R = 0U;
    RTC->BKP8R = 0U;
    RTC->BKP9R = 0U;
    RTC->BKP10R = 0U;
    RTC->BKP11R = 0U;
    RTC->BKP12R = 0U;
    RTC->BKP13R = 0U;
}

void app_gateway_fault_capture_and_reset(uint32_t *stacked_sp,
                                         uint32_t code,
                                         uint32_t exc_return)
{
    uint32_t pc = 0U;
    uint32_t lr = exc_return;

    __disable_irq();

    if (fault_stack_pointer_ok(stacked_sp)) {
        lr = stacked_sp[5];
        pc = stacked_sp[6];
    }

    fault_backup_access_enable();

    uint32_t count = RTC->BKP13R;
    if (RTC->BKP3R != APP_GATEWAY_FAULT_MAGIC ||
        count == 0xFFFFFFFFUL ||
        count > 1000000000UL) {
        count = 0U;
    }

    RTC->BKP4R = code;
    RTC->BKP5R = SCB->CFSR;
    RTC->BKP6R = SCB->HFSR;
    RTC->BKP7R = SCB->DFSR;
    RTC->BKP8R = SCB->AFSR;
    RTC->BKP9R = SCB->MMFAR;
    RTC->BKP10R = SCB->BFAR;
    RTC->BKP11R = pc;
    RTC->BKP12R = lr;
    RTC->BKP13R = count + 1UL;
    RTC->BKP3R = APP_GATEWAY_FAULT_MAGIC;

    __DSB();
    __ISB();
    NVIC_SystemReset();

    while (1) {
    }
}

void app_gateway_print_reset_reason(void)
{
    uint32_t csr = RCC->CSR;
    s_reset_csr = csr;

    print("[RST] CSR=0x%08lX\r\n", (unsigned long)csr);

    if (csr & RCC_CSR_PORRSTF)  print("[RST] POR/PDR reset\r\n");
    if (csr & RCC_CSR_BORRSTF)  print("[RST] BOR reset\r\n");
    if (csr & RCC_CSR_PINRSTF)  print("[RST] PIN reset (NRST)\r\n");
    if (csr & RCC_CSR_SFTRSTF)  print("[RST] Software reset\r\n");
    if (csr & RCC_CSR_IWDGRSTF) print("[RST] IWDG reset\r\n");
    if (csr & RCC_CSR_WWDGRSTF) print("[RST] WWDG reset\r\n");
    if (csr & RCC_CSR_LPWRRSTF) print("[RST] Low power reset\r\n");

    __HAL_RCC_CLEAR_RESET_FLAGS();
}

uint32_t app_gateway_reset_csr(void)
{
    return s_reset_csr;
}

const char *app_gateway_reset_reason_name(void)
{
    return reset_reason_from_csr(s_reset_csr);
}

void app_gateway_flash_setup(W25Q_Handle *flash,
                             SPI_HandleTypeDef *hspi,
                             GPIO_TypeDef *cs_port,
                             uint16_t cs_pin)
{
    if (!flash) return;

    flash->hspi = hspi;
    flash->cs_port = cs_port;
    flash->cs_pin = cs_pin;
}

void app_gateway_flash_test(W25Q_Handle *flash)
{
    if (!flash) return;

    uint32_t id = w25q_read_jedec_id(flash);
    print("JEDEC raw = 0x%06lX\r\n", (unsigned long)id);

    if (!w25q_init(flash))
    {
        print("W25Q init FAIL\r\n");
        return;
    }

    id = w25q_read_jedec_id(flash);

    print("W25Q JEDEC: %02lX %02lX %02lX (0x%06lX)\r\n",
          (id >> 16) & 0xFF,
          (id >> 8) & 0xFF,
          id & 0xFF,
          (unsigned long)id);
}

void app_gateway_basepri_recover(void)
{
    print("[MAIN] before BASEPRI clear PRIMASK=%lu BASEPRI=%lu tick=%lu\r\n",
          (unsigned long)__get_PRIMASK(),
          (unsigned long)__get_BASEPRI(),
          (unsigned long)HAL_GetTick());

    __set_BASEPRI(0);
    __enable_irq();

    print("[MAIN] after BASEPRI clear PRIMASK=%lu BASEPRI=%lu tick=%lu\r\n",
          (unsigned long)__get_PRIMASK(),
          (unsigned long)__get_BASEPRI(),
          (unsigned long)HAL_GetTick());
}
