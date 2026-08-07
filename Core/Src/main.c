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

#include "debug_uart.h"
#include "fatfs.h"

/* Board / platform */
#include "app_board_io.h"
#include "app_debug_led.h"
#include "app_firmware.h"
#include "app_gateway_platform.h"
#include "app_gateway_runtime.h"
#include "app_sd_card.h"
#include "app_watchdog.h"

/* Modbus */
#include "rs485.h"
#include "modbus_rtu_master.h"
#include "modbus_config_json.h"
#include "app_modbus_payload.h"

/* RTC / flash */
#include "rtc_manager.h"
#include "w25q16.h"
#include "app_flash_log.h"

/* GSM / MQTT */
#include "uart2_rx_engine.h"
#include "mqtt_handler.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
char topic_d2c_telemetry[64];
char topic_c2d_commands[64];
char topic_d2c_message[64];
char topic_d2c_logs[64];
char topic_d2c_health[64];

uint8_t buffer[2048];
uart2_rx_t g_u2rx;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define MODEM_UART_HANDLE     (&huart6)
#define MODEM_UART_INSTANCE   USART6
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
DMA_HandleTypeDef hdma_usart6_rx;
DMA_HandleTypeDef hdma_usart6_tx;

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 1024 * 16,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Definitions for ModbusTask */
osThreadId_t ModbusTaskHandle;
const osThreadAttr_t ModbusTask_attributes = {
  .name = "ModbusTask",
  .stack_size = 512 * 8,
  .priority = (osPriority_t) osPriorityNormal,
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
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_ADC1_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_SPI1_Init(void);
static void MX_SDIO_SD_Init(void);
static void MX_USART6_UART_Init(void);
void StartDefaultTask(void *argument);
void StartTask02(void *argument);

/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
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

    print("CFG telemetryInterval=%lu s\r\n",
          (unsigned long)cfg->telemetry_interval_s);

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

static void app_store_device_boot_log(uint32_t boot_count)
{
    char message[96];
    uint32_t csr = app_gateway_reset_csr();
    const char *reason = app_gateway_reset_reason_name();
    uint8_t rtc_ok = rtc_is_datetime_valid();
    uint8_t time_preserved = rtc_time_was_preserved();

    if (!app_modbus_payload_has_valid_config())
    {
        print("[BOOT_LOG] skipped: waiting for valid config/apiKey reason=%s bootCount=%lu\r\n",
              reason,
              (unsigned long)boot_count);
        return;
    }

    snprintf(message,
             sizeof(message),
             "device booted fw=%s reason=%s csr=0x%08lX time_preserved=%s rtc=%s",
             APP_FIRMWARE_VERSION,
             reason,
             (unsigned long)csr,
             time_preserved ? "true" : "false",
             rtc_ok ? "ok" : "fail");

    uint8_t ok = app_flash_log_write(APP_LOG_ERROR,
                                     "SYSTEM",
                                     "DEVICE_BOOT",
                                     message,
                                     (int32_t)boot_count,
                                     (int32_t)csr,
                                     "NONE");

    print("[BOOT_LOG] stored=%u reason=%s bootCount=%lu csr=0x%08lX time_preserved=%u rtc=%s\r\n",
          ok,
          reason,
          (unsigned long)boot_count,
          (unsigned long)csr,
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
  MX_DMA_Init();
  MX_USART3_UART_Init();
  MX_ADC1_Init();
  MX_USART1_UART_Init();
  MX_SPI1_Init();
  MX_SDIO_SD_Init();
  MX_FATFS_Init();
  MX_USART6_UART_Init();

  /* USER CODE BEGIN 2 */
  debug_uart_init(&huart1);
  debug_uart_set_sink(app_sd_terminal_log_sink);
  debug_uart_force_enabled_for(60000U);
  HAL_Delay(100);

  app_gateway_print_reset_reason();
  print("\r\n=== Device boot ===\r\n");
  app_print_firmware_identity("BOOT");

  app_debug_led_init();

  if (rtc_clock_configure_with_fallback() == HAL_OK)
  {
      print("[RTC] Clock source set OK\r\n");
  }
  else
  {
      print("[RTC][ERR] Clock source config failed\r\n");
  }

  rtc_init_with_bkp_once();
  if (!rtc_sync_to_code_time_once()) {
      rtc_sync_to_build_time_if_newer();
  }
  app_print_rtc_boot_time();
  uint32_t boot_count = rtc_increment_boot_count();

  app_sd_terminal_log_enable(1U);
  (void)app_sd_self_test_once();

  app_board_io_init(&hadc1);

  app_gateway_flash_setup(&s_flash, &hspi1, GPIOB, GPIO_PIN_0);
  app_gateway_flash_test(&s_flash);
  MQTT_SetOtaFlash(&s_flash);

  modbus_cfg_t cfg_from_flash;
  bool cfg_ok = modbus_cfg_load(&cfg_from_flash);

  print("Load from FLASH: %s\r\n", cfg_ok ? "VALID" : "DEFAULTS");
  app_print_loaded_config(&cfg_from_flash);
  app_modbus_payload_set_cfg(&cfg_from_flash);
  debug_uart_set_enabled(modbus_cfg_debug_print_enabled(&cfg_from_flash));
  print("[DEBUG] UART print %s\r\n", debug_uart_is_enabled() ? "enabled" : "disabled");

  print("[MAIN] before app_flash_log_init\r\n");
  app_flash_log_init(&s_flash);
  print("[MAIN] after app_flash_log_init pending=%u total=%u dropped=%lu deduped=%lu\r\n",
        app_flash_log_pending_count(),
        app_flash_log_total_count(),
        (unsigned long)app_flash_log_dropped_count(),
        (unsigned long)app_flash_log_deduped_count());
  app_store_device_boot_log(boot_count);
  app_store_pending_fault_log();
  app_store_sd_card_boot_log();

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

  app_watchdog_start();
  print("[IWDG] started\r\n");

  osKernelInitialize();

  /* USER CODE BEGIN RTOS_INIT */
  u2RxSemHandle = osSemaphoreNew(1, 0, NULL);
  u2rx_init(&g_u2rx, MODEM_UART_HANDLE, u2RxSemHandle);

  if (u2rx_start(&g_u2rx) != HAL_OK)
  {
      print("[UART6][ERR] DMA-to-IDLE start failed\r\n");
  }
  else
  {
      print("[UART6] DMA-to-IDLE started\r\n");
  }

  mbRxSemHandle = osSemaphoreNew(1, 0, NULL);

  __HAL_RCC_GPIOD_CLK_ENABLE();
  rs485_init(&g_rs485, GPIOD, GPIO_PIN_10);
  mbm_init(&g_mbm, &huart3, &g_rs485, mb_rx_buf, sizeof(mb_rx_buf), mbRxSemHandle);

  app_modbus_payload_set_cfg(&cfg_from_flash);
  app_modbus_payload_init(&g_mbm);
//  app_relay_test_sequence();
  /* USER CODE END RTOS_INIT */

  /* Create the thread(s) */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);
  ModbusTaskHandle = osThreadNew(StartTask02, NULL, &ModbusTask_attributes);

  app_watchdog_register_task(APP_WATCHDOG_TASK_RUNTIME);
  app_watchdog_register_task(APP_WATCHDOG_TASK_LED);

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
  RCC_OscInitStruct.PLL.PLLQ = 4;
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
  hspi1.Instance = SPI1;
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
  hsd.Init.BusWide = SDIO_BUS_WIDE_1B;
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
  huart1.Instance = USART1;
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
  huart3.Instance = USART3;
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

/**
  * @brief USART6 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART6_UART_Init(void)
{
  huart6.Instance = USART6;
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

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{
  __HAL_RCC_DMA1_CLK_ENABLE();
  __HAL_RCC_DMA2_CLK_ENABLE();

  HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);

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

  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOD, GPIO_PIN_12|GPIO_PIN_10|GPIO_PIN_3, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOD, GPIO_PIN_3, GPIO_PIN_SET);

  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_12|GPIO_PIN_3;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);
}

/* USER CODE BEGIN 4 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart->Instance == USART3)
    {
        mbm_on_rx_event(&g_mbm, huart, Size);
        return;
    }

    if (huart->Instance == MODEM_UART_INSTANCE)
    {
        u2rx_on_idle_event(&g_u2rx, Size);

        __HAL_UART_CLEAR_OREFLAG(huart);
        huart->RxState = HAL_UART_STATE_READY;
        huart->ErrorCode = HAL_UART_ERROR_NONE;

        memset(g_u2rx.dma_buf, 0, U2_DMA_RX_SZ);

        if (HAL_UARTEx_ReceiveToIdle_DMA(MODEM_UART_HANDLE,
                                         g_u2rx.dma_buf,
                                         U2_DMA_RX_SZ) != HAL_OK)
        {
            HAL_UART_AbortReceive(huart);

            (void)HAL_UARTEx_ReceiveToIdle_DMA(MODEM_UART_HANDLE,
                                               g_u2rx.dma_buf,
                                               U2_DMA_RX_SZ);
        }
    }
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
        app_watchdog_refresh();
        osDelay(1000);
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
