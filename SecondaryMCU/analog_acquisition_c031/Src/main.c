#include "main.h"

#include <string.h>

#define SAMPLE_INTERVAL_MS  50U
#define ADC_CHANNEL_COUNT    11U
#define LINK_SOF_0           0xA5U
#define LINK_SOF_1           0x5AU
#define LINK_VERSION         1U
#define LINK_MESSAGE_SAMPLES 1U
#define LINK_PAYLOAD_LENGTH  (4U + 4U + 1U + (ADC_CHANNEL_COUNT * 2U) + 2U)
#define LINK_FRAME_LENGTH    (2U + 1U + 1U + 1U + LINK_PAYLOAD_LENGTH + 2U)

ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;
UART_HandleTypeDef huart1;

static uint32_t sequence;
static volatile uint8_t adc_scan_complete;
static uint8_t adc_dma_started;

static void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_USART1_UART_Init(void);
static uint16_t crc16(const uint8_t *data, uint16_t length);
static uint8_t sample_all(uint16_t raw[ADC_CHANNEL_COUNT]);
static void send_scan(const uint16_t raw[ADC_CHANNEL_COUNT], uint16_t status);

int main(void)
{
  uint32_t next_sample;
  uint16_t raw[ADC_CHANNEL_COUNT] = {0};

  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_USART1_UART_Init();

  if (HAL_ADCEx_Calibration_Start(&hadc1) != HAL_OK) {
    Error_Handler();
  }

  next_sample = HAL_GetTick();
  for (;;) {
    uint32_t now = HAL_GetTick();
    if ((int32_t)(now - next_sample) >= 0) {
      uint16_t status = sample_all(raw) ? 0U : 1U;
      send_scan(raw, status);
      next_sample += SAMPLE_INTERVAL_MS;
      if ((int32_t)(now - next_sample) >= 0) {
        next_sample = now + SAMPLE_INTERVAL_MS;
      }
    }
  }
}

static uint8_t sample_all(uint16_t raw[ADC_CHANNEL_COUNT])
{
  uint32_t start = HAL_GetTick();

  adc_scan_complete = 0U;
  if (!adc_dma_started) {
    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)raw, ADC_CHANNEL_COUNT) != HAL_OK) {
      return 0U;
    }
    adc_dma_started = 1U;
  } else if (HAL_ADC_Start(&hadc1) != HAL_OK) {
    return 0U;
  }

  while (!adc_scan_complete && ((HAL_GetTick() - start) < 5U)) {}
  return adc_scan_complete;
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc == &hadc1) adc_scan_complete = 1U;
}

static void put_u16_le(uint8_t *dst, uint16_t value)
{
  dst[0] = (uint8_t)value;
  dst[1] = (uint8_t)(value >> 8);
}

static void put_u32_le(uint8_t *dst, uint32_t value)
{
  dst[0] = (uint8_t)value;
  dst[1] = (uint8_t)(value >> 8);
  dst[2] = (uint8_t)(value >> 16);
  dst[3] = (uint8_t)(value >> 24);
}

static void send_scan(const uint16_t raw[ADC_CHANNEL_COUNT], uint16_t status)
{
  uint8_t frame[LINK_FRAME_LENGTH];
  uint16_t offset = 0U;
  uint16_t checksum;

  frame[offset++] = LINK_SOF_0;
  frame[offset++] = LINK_SOF_1;
  frame[offset++] = LINK_VERSION;
  frame[offset++] = LINK_MESSAGE_SAMPLES;
  frame[offset++] = LINK_PAYLOAD_LENGTH;
  put_u32_le(&frame[offset], sequence++);
  offset += 4U;
  put_u32_le(&frame[offset], HAL_GetTick());
  offset += 4U;
  frame[offset++] = ADC_CHANNEL_COUNT;
  for (uint32_t i = 0U; i < ADC_CHANNEL_COUNT; ++i) {
    put_u16_le(&frame[offset], raw[i]);
    offset += 2U;
  }
  put_u16_le(&frame[offset], status);
  offset += 2U;

  checksum = crc16(&frame[2], (uint16_t)(offset - 2U));
  put_u16_le(&frame[offset], checksum);
  offset += 2U;
  (void)HAL_UART_Transmit(&huart1, frame, offset, 10U);
}

static uint16_t crc16(const uint8_t *data, uint16_t length)
{
  uint16_t crc = 0xFFFFU;
  for (uint16_t i = 0U; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      crc = (crc & 1U) ? (uint16_t)((crc >> 1U) ^ 0xA001U) : (uint16_t)(crc >> 1U);
    }
  }
  return crc;
}

static void SystemClock_Config(void)
{
  RCC_OscInitTypeDef osc = {0};
  RCC_ClkInitTypeDef clock = {0};

  osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  osc.HSIState = RCC_HSI_ON;
  osc.HSIDiv = RCC_HSI_DIV1;
  osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) Error_Handler();

  clock.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
  clock.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clock.APB1CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_1) != HAL_OK) Error_Handler();
}

static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef gpio = {0};
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 |
             GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7 |
             GPIO_PIN_8 | GPIO_PIN_11 | GPIO_PIN_12;
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &gpio);

  gpio.Pin = GPIO_PIN_14;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  gpio.Alternate = GPIO_AF0_USART1;
  HAL_GPIO_Init(GPIOC, &gpio);

  gpio.Pin = GPIO_PIN_7;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Alternate = GPIO_AF0_USART1;
  HAL_GPIO_Init(GPIOB, &gpio);
}

static void MX_ADC1_Init(void)
{
  static const uint32_t channels[ADC_CHANNEL_COUNT] = {
    ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3,
    ADC_CHANNEL_4, ADC_CHANNEL_5, ADC_CHANNEL_6, ADC_CHANNEL_7,
    ADC_CHANNEL_8, ADC_CHANNEL_11, ADC_CHANNEL_12
  };
  ADC_ChannelConfTypeDef config = {0};

  __HAL_RCC_ADC_CLK_ENABLE();
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV2;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_SEQ_FIXED;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.LowPowerAutoPowerOff = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1U;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = ENABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  hadc1.Init.SamplingTimeCommon1 = ADC_SAMPLETIME_39CYCLES_5;
  hadc1.Init.SamplingTimeCommon2 = ADC_SAMPLETIME_39CYCLES_5;
  hadc1.Init.OversamplingMode = DISABLE;
  hadc1.Init.TriggerFrequencyMode = ADC_TRIGGER_FREQ_HIGH;
  __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);
  if (HAL_ADC_Init(&hadc1) != HAL_OK) Error_Handler();

  config.Rank = ADC_RANK_CHANNEL_NUMBER;
  config.SamplingTime = ADC_SAMPLINGTIME_COMMON_1;
  for (uint32_t i = 0U; i < ADC_CHANNEL_COUNT; ++i) {
    config.Channel = channels[i];
    if (HAL_ADC_ConfigChannel(&hadc1, &config) != HAL_OK) Error_Handler();
  }
}

static void MX_DMA_Init(void)
{
  __HAL_RCC_DMA1_CLK_ENABLE();

  hdma_adc1.Instance = DMA1_Channel1;
  hdma_adc1.Init.Request = DMA_REQUEST_ADC1;
  hdma_adc1.Init.Direction = DMA_PERIPH_TO_MEMORY;
  hdma_adc1.Init.PeriphInc = DMA_PINC_DISABLE;
  hdma_adc1.Init.MemInc = DMA_MINC_ENABLE;
  hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  hdma_adc1.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
  hdma_adc1.Init.Mode = DMA_CIRCULAR;
  hdma_adc1.Init.Priority = DMA_PRIORITY_HIGH;
  if (HAL_DMA_Init(&hdma_adc1) != HAL_OK) Error_Handler();

  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 1U, 0U);
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);
}

static void MX_USART1_UART_Init(void)
{
  __HAL_RCC_USART1_CLK_ENABLE();
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart1) != HAL_OK) Error_Handler();
}

void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}
