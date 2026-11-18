/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : STM32F407 + FreeRTOS gateway main entry
  *
  *                  Keep CubeMX-generated peripheral init in this file.
  *                  Application runtime is moved to app_gateway_runtime.*
  *                  Board/platform helpers are moved to app_gateway_platform.*
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_profile.h"
#include "cJSON.h"
#include "debug_uart.h"
#include "usb_device.h"
#include "usbd_cdc_if.h"
#include "fatfs.h"
#include "FreeRTOS.h"

/* Board / platform */
#include "app_board_io.h"
#include "app_debug_led.h"
#include "app_firmware.h"
#include "app_gateway_platform.h"
#include "app_health.h"
#include "app_gateway_runtime.h"
#include "app_sd_card.h"
#include "app_watchdog.h"
#include "connectivity.h"
#include "secondary_adc_link.h"

/* Modbus */
#include "rs485.h"
#include "modbus_rtu_master.h"
#include "modbus_config_json.h"
#include "app_modbus_payload.h"

/* RTC / flash */
#include "rtc_manager.h"
#include "w25q16.h"
#include "app_flash_log.h"

/* Cellular modem */
#include "uart2_rx_engine.h"
#include "modem_urc.h"
#include "mqtt_handler.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
char topic_d2c_telemetry[64];
char topic_c2d_commands[64];
char topic_d2c_message[64];
char topic_d2c_logs[64];
char topic_d2c_health[64];
char topic_d2c_event[64];
uint32_t app_boot_count;
char app_boot_id[48] = "unknown";
char app_reset_reason[40] = "UNKNOWN";

uart2_rx_t g_u2rx;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define USB_CDC_STANDALONE_TEST  0U
#define USB_CDC_BOOT_GRACE_MS     1000U
#define SECONDARY_ADC_LINK_TEST   0U
#define APP_RUNTIME_WATCHDOG_ENABLED 1U

#ifndef BOARD_MODBUS_DIAGNOSTIC_ONLY
#define BOARD_MODBUS_DIAGNOSTIC_ONLY 0U
#endif

#if BOARD_HAS_CELLULAR
#define MODEM_UART_HANDLE     (&huart6)
#define MODEM_UART_INSTANCE   BOARD_CELLULAR_UART_INSTANCE
#endif
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
RTC_HandleTypeDef hrtc;
SD_HandleTypeDef hsd;
SPI_HandleTypeDef hspi1;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart3;
UART_HandleTypeDef huart6;
DMA_HandleTypeDef hdma_usart3_rx;
DMA_HandleTypeDef hdma_adc1;
DMA_HandleTypeDef hdma_usart6_rx;
DMA_HandleTypeDef hdma_usart6_tx;

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 1024 * 10,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Definitions for ModbusTask */
osThreadId_t ModbusTaskHandle;
const osThreadAttr_t ModbusTask_attributes = {
  .name = "ModbusTask",
  .stack_size = 512 * 10,
  .priority = (osPriority_t) osPriorityNormal,
};

osThreadId_t WatchdogTaskHandle;
const osThreadAttr_t WatchdogTask_attributes = {
  .name = "WatchdogTask",
  .stack_size = 512 * 2,
  .priority = (osPriority_t) osPriorityAboveNormal,
};

/* USER CODE BEGIN PV */
static uint8_t mb_rx_buf[256];

osSemaphoreId_t mbRxSemHandle;
osSemaphoreId_t u2RxSemHandle;

static rs485_t g_rs485;
static mbm_t   g_mbm;
static W25Q_Handle s_flash;

osMutexId_t g_modemMutex;
const osMutexAttr_t g_modemMutexAttr = { .name = "modemMutex" };

osEventFlagsId_t g_sysEvt;
#define EVT_MQTT_READY (1u << 0)

volatile uint32_t g_uplink_pause_until = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void StartWatchdogTask(void *argument);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_ADC1_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_SPI1_Init(void);
static void MX_SDIO_SD_Init(void);
#if BOARD_HAS_CELLULAR
static void MX_USART6_UART_Init(void);
#endif
void StartDefaultTask(void *argument);
void StartTask02(void *argument);
#if BOARD_MODBUS_DIAGNOSTIC_ONLY
static void StartModbusDiagnosticTask(void *argument);
static void app_run_modbus_diagnostic_only(void);
#endif

/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static rtc_boot_stage_t s_previous_boot_stage = RTC_BOOT_STAGE_NONE;

#if BOARD_MODBUS_DIAGNOSTIC_ONLY
static volatile uint32_t s_modbus_diag_uart_error = 0U;

static void StartModbusDiagnosticTask(void *argument)
{
  (void)argument;
  uint16_t reg = 0U;
  uint32_t cycle = 0U;

  print("[MODBUS_TEST] task started\r\n");
  for (;;) {
    cycle++;
    s_modbus_diag_uart_error = 0U;
    memset(mb_rx_buf, 0, sizeof(mb_rx_buf));

    HAL_StatusTypeDef cfg_st = mbm_configure_serial(&g_mbm, 9600U, 8U, 1U, 0U);
    HAL_StatusTypeDef read_st = HAL_ERROR;
    if (cfg_st == HAL_OK) {
      read_st = mbm_read_holding_03(&g_mbm, 1U, 0U, 1U, 1000U, &reg);
    }

    print("[MODBUS_TEST] cycle=%lu request=01/03/0000/0001 cfg=%s result=%s rx_len=%u uart_err=0x%08lX de=%s",
          (unsigned long)cycle,
          (cfg_st == HAL_OK) ? "OK" : "ERROR",
          (read_st == HAL_OK) ? "OK" : ((read_st == HAL_TIMEOUT) ? "TIMEOUT" : "ERROR"),
          (unsigned)g_mbm.rx_len,
          (unsigned long)(huart3.ErrorCode | s_modbus_diag_uart_error),
          (HAL_GPIO_ReadPin(BOARD_MODBUS_DE_GPIO_PORT, BOARD_MODBUS_DE_GPIO_PIN) == GPIO_PIN_SET) ? "TX" : "RX");

    if (g_mbm.rx_len > 0U) {
      uint16_t shown = g_mbm.rx_len;
      if (shown > 32U) shown = 32U;
      print(" raw=");
      for (uint16_t i = 0U; i < shown; i++) {
        print("%02X", g_mbm.rx_buf[i]);
        if ((i + 1U) < shown) print(" ");
      }
    }

    if (read_st == HAL_OK) {
      print(" value=%u", (unsigned)reg);
    }
    print("\r\n");
    osDelay(1000U);
  }
}

static void app_run_modbus_diagnostic_only(void)
{
  static const osThreadAttr_t test_task_attributes = {
    .name = "ModbusDiag",
    .stack_size = 2048U,
    .priority = (osPriority_t)osPriorityNormal,
  };

  print("\r\n[MODBUS_TEST] B1-V4-C isolated test\r\n");
  print("[MODBUS_TEST] UART3 TX=PD8 RX=PD9 DE=PD10, slave=1 baud=9600 8N1 FC=03 start=0 qty=1\r\n");

  osKernelInitialize();
  mbRxSemHandle = osSemaphoreNew(1U, 0U, NULL);
  rs485_init(&g_rs485, BOARD_MODBUS_DE_GPIO_PORT, BOARD_MODBUS_DE_GPIO_PIN);
  mbm_init(&g_mbm, &huart3, &g_rs485, mb_rx_buf, sizeof(mb_rx_buf), mbRxSemHandle);

  osThreadId_t task = osThreadNew(StartModbusDiagnosticTask, NULL, &test_task_attributes);
  print("[MODBUS_TEST] semaphore=%s task=%s\r\n",
        (mbRxSemHandle != NULL) ? "OK" : "FAIL",
        (task != NULL) ? "OK" : "FAIL");
  osKernelStart();

  print("[MODBUS_TEST][FATAL] scheduler returned\r\n");
  for (;;) HAL_Delay(1000U);
}
#endif

#if SECONDARY_ADC_LINK_TEST
static void app_secondary_adc_link_test(void)
{
  uint32_t last_print_ms = HAL_GetTick();
  secondary_adc_sample_t latest_sample = {0};
  bool have_sample = false;

  print("\r\n[ADC_LINK_TEST] raw ADC monitor\r\n");

  if (!secondary_adc_link_start(&huart1)) {
    print("[ADC_LINK_TEST][ERR] UART receive start failed\r\n");
    for (;;) HAL_Delay(1000U);
  }

  for (;;) {
    secondary_adc_sample_t sample;
    uint32_t now = HAL_GetTick();

    while (secondary_adc_link_pop(&sample)) {
      latest_sample = sample;
      have_sample = true;
    }

    now = HAL_GetTick();
    if (have_sample && ((now - last_print_ms) >= 500U)) {
      char line[128];
      size_t used = 0U;
      int written = snprintf(line, sizeof(line), "RAW:");

      if (written > 0) {
        used = (size_t)written;
      }

      for (uint8_t i = 0U;
           (i < latest_sample.channel_count) && (used < sizeof(line));
           i++) {
        written = snprintf(&line[used], sizeof(line) - used,
                           "%s%u", (i == 0U) ? " " : ",",
                           latest_sample.raw[i]);
        if ((written < 0) || ((size_t)written >= (sizeof(line) - used))) {
          used = sizeof(line) - 1U;
          break;
        }
        used += (size_t)written;
      }

      if (used < (sizeof(line) - 2U)) {
        line[used++] = '\r';
        line[used++] = '\n';
        line[used] = '\0';
      }

      print("%s", line);
      last_print_ms = now;
    }

    HAL_Delay(1U);
  }
}
#endif

#if BOARD_HAS_CELLULAR
static void app_cellular_hardware_power_on(void)
{
    uint32_t wait_start;

    /* Keep hardware reset inactive. PWRKEY is an active-low momentary input. */
    HAL_GPIO_WritePin(BOARD_CELLULAR_RESET_GPIO_PORT,
                      BOARD_CELLULAR_RESET_GPIO_PIN,
                      GPIO_PIN_SET);
    HAL_GPIO_WritePin(BOARD_CELLULAR_PWRKEY_GPIO_PORT,
                      BOARD_CELLULAR_PWRKEY_GPIO_PIN,
                      GPIO_PIN_SET);
    HAL_Delay(100U);

    print("[MODEM][HW] PWRKEY pulse start pin=PD15 duration=%u ms reset=inactive\r\n",
          (unsigned int)BOARD_CELLULAR_PWRKEY_PULSE_MS);
    HAL_GPIO_WritePin(BOARD_CELLULAR_PWRKEY_GPIO_PORT,
                      BOARD_CELLULAR_PWRKEY_GPIO_PIN,
                      GPIO_PIN_RESET);

    wait_start = HAL_GetTick();
    while ((HAL_GetTick() - wait_start) < BOARD_CELLULAR_PWRKEY_PULSE_MS) {
        app_watchdog_force_refresh();
        HAL_Delay(100U);
    }

    HAL_GPIO_WritePin(BOARD_CELLULAR_PWRKEY_GPIO_PORT,
                      BOARD_CELLULAR_PWRKEY_GPIO_PIN,
                      GPIO_PIN_SET);
    print("[MODEM][HW] PWRKEY released; waiting %u ms for modem boot\r\n",
          (unsigned int)BOARD_CELLULAR_BOOT_SETTLE_MS);

    wait_start = HAL_GetTick();
    while ((HAL_GetTick() - wait_start) < BOARD_CELLULAR_BOOT_SETTLE_MS) {
        app_watchdog_force_refresh();
        HAL_Delay(100U);
    }

    print("[MODEM][HW] boot settle complete; AT sync may start\r\n");
}
#endif

#if USB_CDC_STANDALONE_TEST
static void app_usb_cdc_standalone_test(void)
{
  uint32_t counter = 0U;

  app_debug_led_init();

  for (;;)
  {
    char message[96];
    int length;

    app_debug_led_boot_pulse();

    length = snprintf(message,
                      sizeof(message),
                      "B1-BETA-C USB CDC TEST OK count=%lu tick=%lu ms\r\n",
                      (unsigned long)counter++,
                      (unsigned long)HAL_GetTick());

    if ((length > 0) &&
        (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED))
    {
      (void)CDC_Transmit_FS((uint8_t *)message, (uint16_t)length);
    }

    HAL_Delay(1000U);
  }
}
#endif

static void app_cjson_use_freertos_heap(void)
{
  cJSON_Hooks hooks = {
    .malloc_fn = pvPortMalloc,
    .free_fn = vPortFree,
  };

  cJSON_InitHooks(&hooks);
}

static void app_print_firmware_identity(const char *tag)
{
  print("[FW][%s] version=%s build=%s model=%s\r\n",
        tag ? tag : "BOOT",
        APP_FIRMWARE_VERSION,
        APP_FIRMWARE_BUILD,
        APP_DEVICE_MODEL);
}

static void app_print_rtc_boot_time(void)
{
    char ts[32];
    uint8_t rtc_ok = rtc_is_datetime_valid();

    rtc_get_datetime_strings(ts, sizeof(ts));

    print("[RTC][BOOT] time=%s preserved=%s valid=%s\r\n",
          ts,
          rtc_time_was_preserved() ? "true" : "false",
          rtc_ok ? "true" : "false");
}

static void app_print_loaded_config(const modbus_cfg_t *cfg)
{
    if (!cfg) return;

    print("CFG device_id=%s config_id=%s imei=%s slaves=%u\r\n",
          cfg->device_id,
          cfg->config_id,
          cfg->imei,
          cfg->slave_count);

    print("CFG telemetryInterval=%lu ms sampleInterval=%lu ms points=%u\r\n",
          (unsigned long)cfg->telemetry_interval_ms,
          (unsigned long)cfg->sample_interval_ms,
          cfg->point_count);

    for (uint8_t si = 0; si < cfg->slave_count; si++)
    {
        const mb_slave_cfg_t *sc = &cfg->slaves[si];

        print("SLAVE[%u] uuid=%s sid=%u baud=%lu db=%u sb=%u par=%u interval=%lu timeout=%lu retries=%u reads=%u\r\n",
              si,
              sc->unique_slave_id,
              sc->slave_id,
              (unsigned long)sc->baud,
              sc->data_bits,
              sc->stop_bits,
              sc->parity,
              (unsigned long)sc->interval_ms,
              (unsigned long)sc->timeout_ms,
              sc->retries,
              sc->read_count);

        for (uint8_t ri = 0; ri < sc->read_count; ri++)
        {
            const mb_read_item_t *rc = &sc->reads[ri];

            print("  READ[%u] readId=%s fn=%u start=%u qty=%u\r\n",
                  ri,
                  rc->read_id,
                  rc->func,
                  rc->start_reg,
                  rc->qty);
        }
    }
}

static void board_gpio_enable_clock(GPIO_TypeDef *port)
{
  if (port == GPIOA) {
    __HAL_RCC_GPIOA_CLK_ENABLE();
  } else if (port == GPIOB) {
    __HAL_RCC_GPIOB_CLK_ENABLE();
  } else if (port == GPIOC) {
    __HAL_RCC_GPIOC_CLK_ENABLE();
  } else if (port == GPIOD) {
    __HAL_RCC_GPIOD_CLK_ENABLE();
  } else if (port == GPIOE) {
    __HAL_RCC_GPIOE_CLK_ENABLE();
  }
}

static void app_boot_stage_enter(rtc_boot_stage_t stage)
{
    rtc_boot_stage_mark(stage);
    print("[BOOT_STAGE] current=%s\r\n", rtc_boot_stage_name(stage));
    app_debug_led_boot_pulse();
    app_watchdog_force_refresh();
}

static void app_store_device_boot_log(uint32_t boot_count)
{
    char message[128];
    uint32_t csr = app_gateway_reset_csr();
    const char *reason = app_gateway_reset_reason_name();
    uint8_t rtc_ok = rtc_is_datetime_valid();
    uint8_t time_preserved = rtc_time_was_preserved();
    const char *prev_stage = rtc_boot_stage_name(s_previous_boot_stage);

    if (!app_modbus_payload_has_valid_config())
    {
        print("[BOOT_LOG] skipped: waiting for valid config/apiKey reason=%s bootCount=%lu\r\n",
              reason,
              (unsigned long)boot_count);
        return;
    }

    snprintf(message,
             sizeof(message),
             "device booted fw=%s reason=%s csr=0x%08lX stage=%s time_preserved=%s rtc=%s",
             APP_FIRMWARE_VERSION,
             reason,
             (unsigned long)csr,
             prev_stage,
             time_preserved ? "true" : "false",
             rtc_ok ? "ok" : "fail");

    uint8_t ok = app_flash_log_write(APP_LOG_ERROR,
                                     "SYSTEM",
                                     "DEVICE_BOOT",
                                     message,
                                     (int32_t)boot_count,
                                     (int32_t)csr,
                                     "NONE");

    print("[BOOT_LOG] stored=%u reason=%s bootCount=%lu csr=0x%08lX prev_stage=%s time_preserved=%u rtc=%s\r\n",
          ok,
          reason,
          (unsigned long)boot_count,
          (unsigned long)csr,
          prev_stage,
          time_preserved,
          rtc_ok ? "ok" : "fail");
}

static void app_store_pending_fault_log(void)
{
    app_gateway_fault_record_t fault;

    if (!app_gateway_fault_peek(&fault))
    {
        return;
    }

    if (!app_modbus_payload_has_valid_config())
    {
        print("[FAULT_LOG] pending fault retained: waiting for valid config/apiKey code=%lu\r\n",
              (unsigned long)fault.code);
        return;
    }

    char message[96];
    snprintf(message,
             sizeof(message),
             "mcu fault=%s pc=0x%08lX lr=0x%08lX cfsr=0x%08lX hfsr=0x%08lX",
             app_gateway_fault_name(fault.code),
             (unsigned long)fault.pc,
             (unsigned long)fault.lr,
             (unsigned long)fault.cfsr,
             (unsigned long)fault.hfsr);

    uint8_t ok = app_flash_log_write(APP_LOG_ERROR,
                                     "SYSTEM",
                                     "MCU_FAULT_RESET",
                                     message,
                                     (int32_t)fault.code,
                                     (int32_t)fault.count,
                                     "MCU_RESET");

    if (ok)
    {
        app_gateway_fault_clear();
    }

    print("[FAULT_LOG] stored=%u fault=%s count=%lu pc=0x%08lX lr=0x%08lX cfsr=0x%08lX hfsr=0x%08lX\r\n",
          ok,
          app_gateway_fault_name(fault.code),
          (unsigned long)fault.count,
          (unsigned long)fault.pc,
          (unsigned long)fault.lr,
          (unsigned long)fault.cfsr,
          (unsigned long)fault.hfsr);
}

static void app_store_sd_card_boot_log(void)
{
    if (app_sd_is_available())
    {
        return;
    }

    if (!app_modbus_payload_has_valid_config())
    {
        print("[SD_LOG] cloud log skipped: waiting for valid config/apiKey\r\n");
        return;
    }

    uint8_t ok = app_flash_log_write(APP_LOG_ERROR,
                                     "STORAGE",
                                     "SD_CARD_NOT_MOUNTED",
                                     "SD card not mounted, local SD logging disabled",
                                     0,
                                     0,
                                     "NONE");

    print("[SD_LOG] boot cloud log stored=%u event=SD_CARD_NOT_MOUNTED\r\n", ok);
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_USB_DEVICE_Init();

#if USB_CDC_STANDALONE_TEST
  app_usb_cdc_standalone_test();
#endif

  MX_DMA_Init();
#if SECONDARY_ADC_LINK_TEST || BOARD_AI_SOURCE_SECONDARY_MCU
  MX_USART1_UART_Init();
#endif
  MX_USART3_UART_Init();
#if !BOARD_AI_SOURCE_SECONDARY_MCU
  MX_ADC1_Init();
#endif
  MX_SPI1_Init();
  MX_SDIO_SD_Init();
  MX_FATFS_Init();
#if BOARD_HAS_CELLULAR
  MX_USART6_UART_Init();
#endif

  /* USER CODE BEGIN 2 */
  app_cjson_use_freertos_heap();
  /* Allow the host to enumerate before the first boot messages are emitted. */
  HAL_Delay(500U);
  debug_uart_init(NULL);
  debug_uart_set_sink(app_sd_terminal_log_sink);
  debug_uart_force_enabled_for(60000U);

  /* Temporary debug aid: allow macOS to recreate the CDC device and give the
   * operator time to reopen the serial terminal before any boot/modem logs. */
  HAL_Delay(USB_CDC_BOOT_GRACE_MS);

#if BOARD_MODBUS_DIAGNOSTIC_ONLY
  app_run_modbus_diagnostic_only();
#endif

  app_gateway_print_reset_reason();
  print("\r\n=== Device boot ===\r\n");
  app_print_firmware_identity("BOOT");

#if SECONDARY_ADC_LINK_TEST
  app_secondary_adc_link_test();
#endif

  app_debug_led_init();

#if BOARD_HAS_CELLULAR
  app_cellular_hardware_power_on();
#endif

  s_previous_boot_stage = rtc_boot_stage_previous();
  print("[BOOT_STAGE] previous=%s\r\n", rtc_boot_stage_name(s_previous_boot_stage));

  app_boot_stage_enter(RTC_BOOT_STAGE_RTC);

  app_watchdog_force_refresh();
  if (rtc_clock_configure_with_fallback() == HAL_OK)
  {
      print("[RTC] Clock source set OK\r\n");
  }
  else
  {
      print("[RTC][ERR] Clock source config failed\r\n");
  }

  app_watchdog_force_refresh();
  rtc_init_with_bkp_once();
  if (!rtc_sync_to_code_time_once()) {
      rtc_sync_to_build_time_if_newer();
  }
  app_print_rtc_boot_time();
  uint32_t boot_count = rtc_increment_boot_count();
  app_boot_count = boot_count;
  /* Boot count plus 32 random bits: unique even if the backup domain (and so
   * the count) is lost, which the platform relies on to detect restarts. */
  snprintf(app_boot_id, sizeof(app_boot_id), "%lu-%08lX", (unsigned long)boot_count,
           (unsigned long)app_gateway_random32());
  snprintf(app_reset_reason, sizeof(app_reset_reason), "%s", app_gateway_reset_reason_name());
  app_health_set_boot_info(app_boot_count, app_boot_id, app_reset_reason);
  app_watchdog_force_refresh();

  app_boot_stage_enter(RTC_BOOT_STAGE_SD_INIT);
  app_sd_terminal_log_enable(1U);
  app_watchdog_force_refresh();
  (void)app_sd_self_test_once();
  app_watchdog_force_refresh();

  app_board_io_init(&hadc1);

  app_boot_stage_enter(RTC_BOOT_STAGE_FLASH_INIT);
  app_gateway_flash_setup(&s_flash,
                          &hspi1,
                          BOARD_W25Q_CS_GPIO_PORT,
                          BOARD_W25Q_CS_GPIO_PIN);
  app_watchdog_force_refresh();
  app_gateway_flash_test(&s_flash);
  connectivity_set_ota_flash(&s_flash);
  modbus_cfg_set_flash(&s_flash);
  app_watchdog_force_refresh();

  app_boot_stage_enter(RTC_BOOT_STAGE_CONFIG_LOAD);
  modbus_cfg_t *cfg_from_flash = modbus_cfg_workspace();
  memset(cfg_from_flash, 0, sizeof(*cfg_from_flash));
  app_watchdog_force_refresh();
  bool cfg_ok = modbus_cfg_load(cfg_from_flash);
  app_watchdog_force_refresh();

  print("Load from FLASH: %s\r\n", cfg_ok ? "VALID" : "DEFAULTS");
  app_print_loaded_config(cfg_from_flash);
  app_modbus_payload_set_cfg(cfg_from_flash);
  /* Relays start in their configured safe state (all OFF unless set). */
  app_relay_apply_safe_states(cfg_ok ? MODBUS_CFG_RELAY_SAFE_ON_MASK(cfg_from_flash) : 0U, "boot");
  /* USB CDC diagnostics are a local firmware setting, not cloud configuration. */
  debug_uart_set_enabled(1U);
  debug_uart_set_at_enabled(1U);
  print("[DEBUG] USB CDC print enabled\r\n");

  app_boot_stage_enter(RTC_BOOT_STAGE_FLASH_LOG_INIT);
  print("[MAIN] before app_flash_log_init\r\n");
  app_watchdog_force_refresh();
  app_flash_log_init(&s_flash);
  app_modbus_payload_set_flash(&s_flash);
  app_watchdog_force_refresh();
  print("[MAIN] after app_flash_log_init pending=%u total=%u dropped=%lu deduped=%lu\r\n",
        app_flash_log_pending_count(),
        app_flash_log_total_count(),
        (unsigned long)app_flash_log_dropped_count(),
        (unsigned long)app_flash_log_deduped_count());
  app_watchdog_force_refresh();
  app_store_device_boot_log(boot_count);
  app_watchdog_force_refresh();
  app_store_pending_fault_log();
  app_watchdog_force_refresh();
  app_store_sd_card_boot_log();
  app_watchdog_force_refresh();

  app_gateway_basepri_recover();
  print("[MAIN] moving to RTOS init\r\n");
  /* USER CODE END 2 */
//  while (1)
//  {
//      app_adc_read_all_mv(ai_mv);
//
//      print("ADC TEST: AI_1=%lu AI_2=%lu AI_3=%lu AI_4=%lu AI_5=%lu AI_6=%lu AI_7=%lu AI_8=%lu AI_9=%lu AI_10=%lu\r\n",
//            (unsigned long)ai_mv[0],
//            (unsigned long)ai_mv[1],
//            (unsigned long)ai_mv[2],
//            (unsigned long)ai_mv[3],
//            (unsigned long)ai_mv[4],
//            (unsigned long)ai_mv[5],
//            (unsigned long)ai_mv[6],
//            (unsigned long)ai_mv[7],
//            (unsigned long)ai_mv[8],
//            (unsigned long)ai_mv[9]);
//
//      HAL_Delay(1000);
//  }
  app_boot_stage_enter(RTC_BOOT_STAGE_RTOS_START);
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_INIT */
#if BOARD_HAS_CELLULAR
  u2RxSemHandle = osSemaphoreNew(1, 0, NULL);
  u2rx_init(&g_u2rx, MODEM_UART_HANDLE, u2RxSemHandle);
  modem_urc_init(&g_u2rx);
  MQTT_InstallFastDownlinkHandler();

  if (u2rx_start(&g_u2rx) != HAL_OK)
  {
      print("[UART6][ERR] DMA-to-IDLE start failed\r\n");
  }
  else
  {
      print("[UART6] DMA-to-IDLE started\r\n");
  }
#endif

  mbRxSemHandle = osSemaphoreNew(1, 0, NULL);

  __HAL_RCC_GPIOD_CLK_ENABLE();
  rs485_init(&g_rs485, BOARD_MODBUS_DE_GPIO_PORT, BOARD_MODBUS_DE_GPIO_PIN);
  mbm_init(&g_mbm, &huart3, &g_rs485, mb_rx_buf, sizeof(mb_rx_buf), mbRxSemHandle);

  app_modbus_payload_set_cfg(cfg_from_flash);
  app_modbus_payload_init(&g_mbm);
#if BOARD_AI_SOURCE_SECONDARY_MCU
  app_adc_set_secondary_uart(&huart1);
#endif
  if (!app_adc_timed_sampler_start())
  {
      print("[ADC][WARN] timed sampler unavailable, blocking AI sampling active\r\n");
  }
//  app_relay_test_sequence();
  /* USER CODE END RTOS_INIT */

  /* Create the thread(s) */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);
  ModbusTaskHandle = osThreadNew(StartTask02, NULL, &ModbusTask_attributes);
  WatchdogTaskHandle = osThreadNew(StartWatchdogTask, NULL, &WatchdogTask_attributes);

  print("[RTOS] tasks runtime=%s modbus=%s watchdog=%s heapFree=%lu\r\n",
        (defaultTaskHandle != NULL) ? "ok" : "FAIL",
        (ModbusTaskHandle != NULL) ? "ok" : "FAIL",
        (WatchdogTaskHandle != NULL) ? "ok" : "FAIL",
        (unsigned long)xPortGetFreeHeapSize());

  if (defaultTaskHandle != NULL)
  {
      app_watchdog_register_task(APP_WATCHDOG_TASK_RUNTIME);
  }
  if (ModbusTaskHandle != NULL)
  {
      app_watchdog_register_task(APP_WATCHDOG_TASK_MODBUS);
  }

  /*
   * Start the runtime watchdog only after its supervisor and monitored tasks
   * exist. Pre-RTOS USB/modem/storage initialization cannot service an RTOS
   * supervisor reliably and must not consume the first watchdog window.
   */
#if APP_RUNTIME_WATCHDOG_ENABLED
  app_watchdog_start();
  print("[IWDG] runtime watchdog started\r\n");
#else
  print("[IWDG] disabled for USB/reset isolation test\r\n");
#endif

  osKernelStart();
  print("Scheduler returned!\r\n");

  while (1)
  {
    HAL_Delay(100);
  }
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_LSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  /* 16 MHz HSI / 8 * 192 / 8 = 48 MHz for USB OTG FS. */
  RCC_OscInitStruct.PLL.PLLQ = 8;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};

  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV2;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_3CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{
  hspi1.Instance = BOARD_W25Q_SPI_INSTANCE;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief SDIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_SDIO_SD_Init(void)
{
  hsd.Instance = SDIO;
  hsd.Init.ClockEdge = SDIO_CLOCK_EDGE_RISING;
  hsd.Init.ClockBypass = SDIO_CLOCK_BYPASS_DISABLE;
  hsd.Init.ClockPowerSave = SDIO_CLOCK_POWER_SAVE_DISABLE;
  hsd.Init.BusWide = BOARD_SDIO_BUS_WIDE;
  hsd.Init.HardwareFlowControl = SDIO_HARDWARE_FLOW_CONTROL_DISABLE;
  hsd.Init.ClockDiv = 118;
}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{
  huart1.Instance = BOARD_DEBUG_UART_INSTANCE;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{
  huart3.Instance = BOARD_MODBUS_UART_INSTANCE;
  huart3.Init.BaudRate = 9600;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
}

#if BOARD_HAS_CELLULAR
/**
  * @brief USART6 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART6_UART_Init(void)
{
  huart6.Instance = BOARD_CELLULAR_UART_INSTANCE;
  huart6.Init.BaudRate = 115200;
  huart6.Init.WordLength = UART_WORDLENGTH_8B;
  huart6.Init.StopBits = UART_STOPBITS_1;
  huart6.Init.Parity = UART_PARITY_NONE;
  huart6.Init.Mode = UART_MODE_TX_RX;
  huart6.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart6.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart6) != HAL_OK)
  {
    Error_Handler();
  }
}
#endif

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{
  __HAL_RCC_DMA1_CLK_ENABLE();
  __HAL_RCC_DMA2_CLK_ENABLE();

  HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);

  HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);

  HAL_NVIC_SetPriority(DMA2_Stream1_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream1_IRQn);

  HAL_NVIC_SetPriority(DMA2_Stream6_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream6_IRQn);
}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  static const board_gpio_output_t startup_outputs[] = BOARD_STARTUP_OUTPUTS_INITIALIZER;

  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

  for (uint8_t i = 0U;
       i < (uint8_t)(sizeof(startup_outputs) / sizeof(startup_outputs[0]));
       i++) {
    board_gpio_enable_clock(startup_outputs[i].port);
    HAL_GPIO_WritePin(startup_outputs[i].port,
                      startup_outputs[i].pin,
                      startup_outputs[i].initial_state);
    GPIO_InitStruct.Pin = startup_outputs[i].pin;
    HAL_GPIO_Init(startup_outputs[i].port, &GPIO_InitStruct);
  }
}

/* USER CODE BEGIN 4 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
#if SECONDARY_ADC_LINK_TEST || BOARD_AI_SOURCE_SECONDARY_MCU
    if (huart->Instance == BOARD_SECONDARY_ADC_UART_INSTANCE)
    {
        secondary_adc_link_uart_rx_event_isr(huart, Size);
        return;
    }
#endif

    if (huart->Instance == BOARD_MODBUS_UART_INSTANCE)
    {
        mbm_on_rx_event(&g_mbm, huart, Size);
        return;
    }

#if BOARD_HAS_CELLULAR
    if (huart->Instance == MODEM_UART_INSTANCE)
    {
        u2rx_on_idle_event(&g_u2rx, Size);
        return;
    }
#endif
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
#if SECONDARY_ADC_LINK_TEST || BOARD_AI_SOURCE_SECONDARY_MCU
    secondary_adc_link_uart_rx_complete_isr(huart);
#else
    (void)huart;
#endif
}

/* Modem UART error accounting (OTA corruption diagnostics). */
volatile uint32_t g_modem_uart_err_count = 0U;
volatile uint32_t g_modem_uart_err_flags = 0U;
volatile uint32_t g_modem_uart_rx_restarts = 0U;

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
#if BOARD_HAS_CELLULAR
    if (huart && huart->Instance == MODEM_UART_INSTANCE) {
        g_modem_uart_err_count++;
        g_modem_uart_err_flags |= huart->ErrorCode;
        /* Blocking errors (overrun) abort the DMA reception; restart it so
         * the modem link does not go silently deaf. */
        if (huart->RxState == HAL_UART_STATE_READY) {
            g_u2rx.dma_pos = 0U;
            if (u2rx_start(&g_u2rx) == HAL_OK) {
                g_modem_uart_rx_restarts++;
            }
        }
        return;
    }
#endif
#if BOARD_MODBUS_DIAGNOSTIC_ONLY
    if (huart && huart->Instance == BOARD_MODBUS_UART_INSTANCE) {
        s_modbus_diag_uart_error |= huart->ErrorCode;
        return;
    }
#endif
#if SECONDARY_ADC_LINK_TEST || BOARD_AI_SOURCE_SECONDARY_MCU
    if (huart && huart->Instance == BOARD_SECONDARY_ADC_UART_INSTANCE) {
        secondary_adc_link_uart_error_isr(huart);
        return;
    }
#endif
    (void)huart;
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    app_modbus_payload_di_edge_isr(GPIO_Pin);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    app_adc_dma_conv_cplt_isr(hadc);
}
/* USER CODE END 4 */

/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
void StartDefaultTask(void *argument)
{
//	app_relay_test_sequence();
    osDelay(3000U);
    app_gateway_runtime_task(argument);
}

/**
  * @brief Function implementing the ModbusTask thread.
  * @param argument: Not used
  * @retval None
  */
void StartTask02(void *argument)
{
    (void)argument;
//    app_relay_test_sequence();
//
//    print("[TASK] ModbusTask entered\r\n");

    for (;;)
    {
        app_watchdog_mark_task(APP_WATCHDOG_TASK_MODBUS);
        app_modbus_payload_sample_due();
        app_watchdog_refresh();
        osDelay(10);
    }
}

static void StartWatchdogTask(void *argument)
{
    (void)argument;

    /* Confirm scheduler entry before applying task-health gating. */
    app_watchdog_force_refresh();

    for (;;)
    {
        /*
         * This task's ability to run is the scheduler liveness check. Keep the
         * hardware watchdog feed independent from application-task heartbeat
         * bookkeeping; those tasks continue to publish their health markers.
         */
        app_watchdog_force_refresh();
        osDelay(250U);
    }
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  app_gateway_fault_capture_and_reset(NULL,
                                      APP_GATEWAY_FAULT_ERROR_HANDLER,
                                      (uint32_t)(uintptr_t)__builtin_return_address(0));
}

#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  (void)file;
  (void)line;
  app_gateway_fault_capture_and_reset(NULL,
                                      APP_GATEWAY_FAULT_ASSERT,
                                      line);
}
#endif /* USE_FULL_ASSERT */
