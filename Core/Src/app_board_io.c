#include "app_board_io.h"

#include "board_profile.h"
#include "stm32f4xx_hal.h"
#include "debug_uart.h"
#include "secondary_adc_link.h"

#include <stdint.h>
#include <string.h>

#ifndef RELAY_FEEDBACK_SETTLE_MS
#define RELAY_FEEDBACK_SETTLE_MS 30U
#endif

static const board_gpio_pin_t g_relays[] = BOARD_RELAY_PINS_INITIALIZER;

#define RELAY_COUNT ((uint8_t)(sizeof(g_relays) / sizeof(g_relays[0])))

#ifndef BOARD_RELAY_FEEDBACK_COUNT
#define BOARD_RELAY_FEEDBACK_COUNT 0U
#endif

#if BOARD_RELAY_FEEDBACK_COUNT > 0U
static const board_gpio_pin_t g_relay_feedback[] = BOARD_RELAY_FEEDBACK_PINS_INITIALIZER;
#endif

static const board_gpio_pin_t g_dis[] = BOARD_DI_PINS_INITIALIZER;

#define DI_COUNT ((uint8_t)(sizeof(g_dis) / sizeof(g_dis[0])))

static const board_adc_channel_t g_ai_channels[] = BOARD_AI_CHANNELS_INITIALIZER;

#define AI_COUNT ((uint8_t)(sizeof(g_ai_channels) / sizeof(g_ai_channels[0])))

#define ADC_MAX_COUNTS 4095U
#define ADC_VREF_MV    3320U
#define APP_ADC_DEBUG_PRINT 0U
#define APP_ADC_SAMPLE_INTERVAL_MS 50UL
#define APP_ADC_SAMPLE_QUEUE_DEPTH 64U

static ADC_HandleTypeDef *s_hadc = NULL;
static UART_HandleTypeDef *s_secondary_adc_uart = NULL;
static volatile uint8_t s_adc_timed_running;
static volatile uint8_t s_adc_sample_w;
static volatile uint8_t s_adc_sample_r;
static volatile uint8_t s_adc_sample_count;
static volatile uint32_t s_adc_total_samples;
static volatile uint32_t s_adc_dropped_samples;
static volatile uint8_t s_adc_latest_valid;
static volatile uint8_t s_power_monitor_valid;
static volatile uint16_t s_power_monitor_raw;
static volatile uint32_t s_power_monitor_sense_mv;
static volatile uint32_t s_power_monitor_supply_mv;
static uint16_t s_adc_dma_raw[AI_COUNT];
static uint16_t s_adc_latest_raw[AI_COUNT];
static uint16_t s_adc_sample_queue[APP_ADC_SAMPLE_QUEUE_DEPTH][AI_COUNT];

#ifndef BOARD_AI_SOURCE_SECONDARY_MCU
#define BOARD_AI_SOURCE_SECONDARY_MCU 0U
#endif

#ifndef BOARD_DI_ACTIVE_LOW
#define BOARD_DI_ACTIVE_LOW 0U
#endif

#ifndef BOARD_DI_GPIO_PULL
#define BOARD_DI_GPIO_PULL GPIO_PULLDOWN
#endif

#ifndef BOARD_RELAY_FEEDBACK_ACTIVE_LOW
#define BOARD_RELAY_FEEDBACK_ACTIVE_LOW 0U
#endif

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

/* ---------- RELAY ---------- */
static void relay_write_raw(uint8_t relay_index, uint8_t on)
{
  if (relay_index >= RELAY_COUNT) {
    return;
  }

#if BOARD_RELAY_ACTIVE_HIGH
  HAL_GPIO_WritePin(g_relays[relay_index].port,
                    g_relays[relay_index].pin,
                    on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#else
  HAL_GPIO_WritePin(g_relays[relay_index].port,
                    g_relays[relay_index].pin,
                    on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#endif
}
static void relay_write(uint8_t relay_index, uint8_t on)
{
    if (relay_index >= RELAY_COUNT) {
        return;
    }

#if BOARD_RELAY_ACTIVE_HIGH
    HAL_GPIO_WritePin(g_relays[relay_index].port,
                      g_relays[relay_index].pin,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#else
    HAL_GPIO_WritePin(g_relays[relay_index].port,
                      g_relays[relay_index].pin,
                      on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#endif
}
static void Relay_GPIO_Init_User(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    board_gpio_enable_clock(g_relays[i].port);
#if BOARD_RELAY_ACTIVE_HIGH
    HAL_GPIO_WritePin(g_relays[i].port, g_relays[i].pin, GPIO_PIN_RESET);
#else
    HAL_GPIO_WritePin(g_relays[i].port, g_relays[i].pin, GPIO_PIN_SET);
#endif
    GPIO_InitStruct.Pin = g_relays[i].pin;
    HAL_GPIO_Init(g_relays[i].port, &GPIO_InitStruct);
    relay_write_raw(i, 0U);
  }

  print("[RELAY] init done board=%s count=%u pins=%s\r\n",
        BOARD_VARIANT_NAME,
        RELAY_COUNT,
        BOARD_RELAY_PIN_LIST);
}

static void Relay_Feedback_GPIO_Init_User(void)
{
#if BOARD_RELAY_FEEDBACK_COUNT > 0U
  GPIO_InitTypeDef gpio = {0};

  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;

  for (uint8_t i = 0U; i < BOARD_RELAY_FEEDBACK_COUNT; i++) {
    board_gpio_enable_clock(g_relay_feedback[i].port);
    gpio.Pin = g_relay_feedback[i].pin;
    HAL_GPIO_Init(g_relay_feedback[i].port, &gpio);
  }

  print("[RELAY] feedback init count=%u pins=%s activeLow=%u\r\n",
        (unsigned)BOARD_RELAY_FEEDBACK_COUNT,
        BOARD_RELAY_FEEDBACK_PIN_LIST,
        (unsigned)BOARD_RELAY_FEEDBACK_ACTIVE_LOW);
#endif
}

static uint8_t relay_feedback_read(uint8_t relay_index)
{
#if BOARD_RELAY_FEEDBACK_COUNT > 0U
  if (relay_index >= BOARD_RELAY_FEEDBACK_COUNT) return 0U;
  GPIO_PinState state = HAL_GPIO_ReadPin(g_relay_feedback[relay_index].port,
                                         g_relay_feedback[relay_index].pin);
#if BOARD_RELAY_FEEDBACK_ACTIVE_LOW
  return (state == GPIO_PIN_RESET) ? 1U : 0U;
#else
  return (state == GPIO_PIN_SET) ? 1U : 0U;
#endif
#else
  (void)relay_index;
  return 0U;
#endif
}

void app_relay_set(uint8_t relay_no, uint8_t on)
{
  if (relay_no == 0U || relay_no > RELAY_COUNT) {
    print("[RELAY][ERR] invalid relay_no=%u\r\n", relay_no);
    return;
  }

  relay_write_raw((uint8_t)(relay_no - 1U), on ? 1U : 0U);

  print("[RELAY] R%u %s\r\n", relay_no, on ? "ON" : "OFF");
}

void app_relay_all_off(void)
{
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    relay_write_raw(i, 0U);
  }

  print("[RELAY] all OFF\r\n");
}

void app_relay_test(uint32_t step_ms)
{
  print("[RELAY] test start step=%lu ms\r\n", (unsigned long)step_ms);

  for (uint8_t i = 0; i < RELAY_COUNT; i++)
  {
    print("[RELAY] R%u ON\r\n", i + 1U);
    relay_write_raw(i, 1U);
    HAL_Delay(step_ms);

    print("[RELAY] R%u OFF\r\n", i + 1U);
    relay_write_raw(i, 0U);
    HAL_Delay(step_ms);
  }

  print("[RELAY] test done\r\n");
}

void app_relay_test_sequence(void)
{
  app_relay_test(1000UL);

  print("[RELAY] all ON\r\n");
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    relay_write_raw(i, 1U);
  }
  HAL_Delay(2000UL);

  app_relay_all_off();
}

/* ---------- DI ---------- */
static void DI_GPIO_Init_User(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING_FALLING;

  /*
   * Keep this as PULLDOWN if your digital input circuit drives HIGH when active.
   * If your optocoupler/input circuit pulls the pin LOW when active, use GPIO_PULLUP instead.
   */
  GPIO_InitStruct.Pull = BOARD_DI_GPIO_PULL;

  for (uint8_t i = 0; i < DI_COUNT; i++) {
    board_gpio_enable_clock(g_dis[i].port);
    GPIO_InitStruct.Pin = g_dis[i].pin;
    HAL_GPIO_Init(g_dis[i].port, &GPIO_InitStruct);
  }

  HAL_NVIC_SetPriority(EXTI9_5_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

static uint8_t di_read_raw(GPIO_TypeDef *port, uint16_t pin)
{
  return (HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_SET) ? 1U : 0U;
}

void app_di_read_raw_all(uint8_t out8[8])
{
  if (!out8) return;

  for (int i = 0; i < 8; i++) {
    out8[i] = 0U;
  }

  for (int i = 0; i < (int)DI_COUNT; i++) {
    out8[i] = di_read_raw(g_dis[i].port, g_dis[i].pin);
  }
}

void app_di_read_all(uint8_t out8[8])
{
  uint8_t raw[8] = {0};

  if (!out8) return;
  app_di_read_raw_all(raw);

  for (int i = 0; i < 8; i++) {
    out8[i] = 0U;
  }

  for (int i = 0; i < (int)DI_COUNT; i++) {
#if BOARD_DI_ACTIVE_LOW
    out8[i] = raw[i] ? 0U : 1U;
#else
    out8[i] = raw[i];
#endif
  }
}

int8_t app_di_index_from_gpio_pin(uint16_t gpio_pin)
{
  for (uint8_t i = 0U; i < DI_COUNT; i++) {
    if (g_dis[i].pin == gpio_pin) return (int8_t)i;
  }
  return -1;
}

/* ---------- ADC ---------- */
static void ADC1_GPIO_Analog_Init_User(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;

  for (uint8_t i = 0; i < AI_COUNT; i++) {
    board_gpio_enable_clock(g_ai_channels[i].port);
    GPIO_InitStruct.Pin = g_ai_channels[i].pin;
    HAL_GPIO_Init(g_ai_channels[i].port, &GPIO_InitStruct);
  }
}

static uint16_t adc1_read_avg(uint32_t channel, uint8_t samples)
{
    if (!s_hadc) return 0xFFFFU;

    ADC_ChannelConfTypeDef sConfig = {0};
    sConfig.Channel      = channel;
    sConfig.Rank         = 1;
    sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;

    if (HAL_ADC_ConfigChannel(s_hadc, &sConfig) != HAL_OK) return 0xFFFFU;

    /* Dummy conversion (discard) */
    HAL_ADC_Start(s_hadc);
    if (HAL_ADC_PollForConversion(s_hadc, 20) != HAL_OK) {
      HAL_ADC_Stop(s_hadc);
      return 0xFFFEU;
    }
    (void)HAL_ADC_GetValue(s_hadc);
    HAL_ADC_Stop(s_hadc);

    uint32_t sum = 0;

    for (uint8_t i = 0; i < samples; i++)
    {
        HAL_ADC_Start(s_hadc);
        if (HAL_ADC_PollForConversion(s_hadc, 20) != HAL_OK) {
          HAL_ADC_Stop(s_hadc);
          return 0xFFFDU;
        }
        sum += HAL_ADC_GetValue(s_hadc);
        HAL_ADC_Stop(s_hadc);
    }

    return (uint16_t)(sum / samples);
}

static uint32_t adc_to_mV(uint16_t raw)
{
  return ((uint32_t)raw * ADC_VREF_MV) / ADC_MAX_COUNTS;
}

static uint32_t adc_raw_to_scaled_mv(uint8_t index, uint16_t raw)
{
  uint32_t pin_mv = adc_to_mV(raw);
  if (index >= AI_COUNT) return pin_mv;
  return (uint32_t)((float)pin_mv * g_ai_channels[index].board_scale);
}

static void power_monitor_update(uint16_t raw)
{
  const uint32_t sense_mv = adc_to_mV(raw);

  s_power_monitor_raw = raw;
  s_power_monitor_sense_mv = sense_mv;
  s_power_monitor_supply_mv =
      (sense_mv * BOARD_POWER_MONITOR_DIVIDER_NUMERATOR) /
      BOARD_POWER_MONITOR_DIVIDER_DENOMINATOR;
  s_power_monitor_valid = 1U;
}

static uint32_t adc_critical_enter(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  return primask;
}

static void adc_critical_exit(uint32_t primask)
{
  if (!primask) __enable_irq();
}

static void adc_timed_queue_reset(void)
{
  uint32_t primask = adc_critical_enter();
  s_adc_sample_w = 0U;
  s_adc_sample_r = 0U;
  s_adc_sample_count = 0U;
  s_adc_total_samples = 0U;
  s_adc_dropped_samples = 0U;
  s_adc_latest_valid = 0U;
  memset(s_adc_dma_raw, 0, sizeof(s_adc_dma_raw));
  memset(s_adc_latest_raw, 0, sizeof(s_adc_latest_raw));
  memset(s_adc_sample_queue, 0, sizeof(s_adc_sample_queue));
  adc_critical_exit(primask);
}

static void adc_timed_trigger_stop(void)
{
  if ((RCC->APB1ENR & RCC_APB1ENR_TIM2EN) != 0U) {
    TIM2->CR1 &= ~TIM_CR1_CEN;
  }
}

static void adc_timed_trigger_start(uint32_t interval_ms)
{
  uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
  uint32_t timclk = pclk1;
  uint32_t prescaler;
  uint32_t period;

  if ((RCC->CFGR & RCC_CFGR_PPRE1) != RCC_CFGR_PPRE1_DIV1) {
    timclk = pclk1 * 2UL;
  }

  prescaler = (timclk / 10000UL);
  if (prescaler == 0UL) prescaler = 1UL;
  prescaler -= 1UL;

  period = (10UL * interval_ms);
  if (period == 0UL) period = 1UL;
  period -= 1UL;

  __HAL_RCC_TIM2_CLK_ENABLE();

  TIM2->CR1 = 0U;
  TIM2->CR2 = 0U;
  TIM2->PSC = prescaler;
  TIM2->ARR = period;
  TIM2->CNT = 0U;
  TIM2->CR2 = TIM_CR2_MMS_1; /* update event as TRGO */
  TIM2->EGR = TIM_EGR_UG;
  TIM2->SR = 0U;
  TIM2->CR1 = TIM_CR1_CEN;
}

static uint8_t adc_config_blocking_single(void)
{
  if (!s_hadc) return 0U;

  s_adc_timed_running = 0U;
  adc_timed_trigger_stop();
  (void)HAL_ADC_Stop_DMA(s_hadc);
  (void)HAL_ADC_Stop(s_hadc);

  s_hadc->Init.ScanConvMode = DISABLE;
  s_hadc->Init.ContinuousConvMode = DISABLE;
  s_hadc->Init.DiscontinuousConvMode = DISABLE;
  s_hadc->Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  s_hadc->Init.ExternalTrigConv = ADC_SOFTWARE_START;
  s_hadc->Init.NbrOfConversion = 1U;
  s_hadc->Init.DMAContinuousRequests = DISABLE;
  s_hadc->Init.EOCSelection = ADC_EOC_SINGLE_CONV;

  return (HAL_ADC_Init(s_hadc) == HAL_OK) ? 1U : 0U;
}

static uint8_t adc_config_timed_scan(void)
{
  if (!s_hadc) return 0U;

  ADC_ChannelConfTypeDef sConfig = {0};

  s_hadc->Init.ScanConvMode = ENABLE;
  s_hadc->Init.ContinuousConvMode = DISABLE;
  s_hadc->Init.DiscontinuousConvMode = DISABLE;
  s_hadc->Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
  s_hadc->Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T2_TRGO;
  s_hadc->Init.NbrOfConversion = AI_COUNT;
  s_hadc->Init.DMAContinuousRequests = ENABLE;
  s_hadc->Init.EOCSelection = ADC_EOC_SEQ_CONV;

  if (HAL_ADC_Init(s_hadc) != HAL_OK) {
    return 0U;
  }

  sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;
  for (uint8_t i = 0U; i < AI_COUNT; i++) {
    sConfig.Channel = g_ai_channels[i].adc_channel;
    sConfig.Rank = (uint32_t)i + 1UL;
    if (HAL_ADC_ConfigChannel(s_hadc, &sConfig) != HAL_OK) {
      return 0U;
    }
  }

  return 1U;
}

void app_adc_read_all_mv(uint32_t out10_mv[10])
{
  if (!out10_mv) return;
  uint16_t latest[AI_COUNT];

  for (uint8_t i = 0; i < 10U; i++) {
    out10_mv[i] = 0U;
  }

#if BOARD_AI_SOURCE_SECONDARY_MCU
  uint32_t primask = adc_critical_enter();
  if (!s_adc_latest_valid) {
    adc_critical_exit(primask);
    return;
  }
  memcpy(latest, s_adc_latest_raw, sizeof(latest));
  adc_critical_exit(primask);

  for (uint8_t i = 0U; i < AI_COUNT && i < 10U; i++) {
    out10_mv[i] = adc_raw_to_scaled_mv(i, latest[i]);
  }
  return;
#else
  if (s_adc_timed_running) {
    uint32_t primask = adc_critical_enter();
    if (!s_adc_latest_valid) {
      adc_critical_exit(primask);
      return;
    }
    memcpy(latest, s_adc_latest_raw, sizeof(latest));
    adc_critical_exit(primask);

    for (uint8_t i = 0U; i < AI_COUNT && i < 10U; i++) {
      out10_mv[i] = adc_raw_to_scaled_mv(i, latest[i]);
    }
    return;
  }

  (void)adc_config_blocking_single();

  for (uint8_t i = 0; i < AI_COUNT && i < 10U; i++) {
    uint16_t raw = adc1_read_avg(g_ai_channels[i].adc_channel, 16);
    uint32_t pin_mv = adc_to_mV(raw);
    uint32_t final_mv = adc_raw_to_scaled_mv(i, raw);

    out10_mv[i] = final_mv;

#if APP_ADC_DEBUG_PRINT
    print("[ADC] AI_%d pin=%s raw=%u pin_mv=%lu final_mv=%lu scale=%ld.%02ld\r\n",
          i + 1,
          g_ai_channels[i].name,
          (unsigned)raw,
          (unsigned long)pin_mv,
          (unsigned long)final_mv,
          (long)g_ai_channels[i].board_scale,
          (long)((g_ai_channels[i].board_scale - (float)((long)g_ai_channels[i].board_scale)) * 100.0f));
#endif
  }
#endif
}

void app_adc_set_secondary_uart(UART_HandleTypeDef *huart)
{
  s_secondary_adc_uart = huart;
}

uint8_t app_adc_timed_sampler_start(void)
{
#if BOARD_AI_SOURCE_SECONDARY_MCU
  if (!s_secondary_adc_uart || AI_COUNT == 0U) return 0U;

  adc_timed_queue_reset();
  if (!secondary_adc_link_start(s_secondary_adc_uart)) {
    print("[ADC][ERR] secondary MCU UART receive start failed\r\n");
    return 0U;
  }

  s_adc_timed_running = 1U;
  print("[ADC] secondary MCU sampler started baud=%lu channels=%u\r\n",
        (unsigned long)BOARD_SECONDARY_ADC_UART_BAUD,
        AI_COUNT);
  return 1U;
#else
  if (!s_hadc || AI_COUNT == 0U) return 0U;

  adc_timed_trigger_stop();
  (void)HAL_ADC_Stop_DMA(s_hadc);
  (void)HAL_ADC_Stop(s_hadc);
  adc_timed_queue_reset();

  if (!adc_config_timed_scan()) {
    (void)adc_config_blocking_single();
    print("[ADC][ERR] timed sampler config failed, using blocking reads\r\n");
    return 0U;
  }

  if (HAL_ADC_Start_DMA(s_hadc, (uint32_t *)s_adc_dma_raw, AI_COUNT) != HAL_OK) {
    (void)adc_config_blocking_single();
    print("[ADC][ERR] DMA start failed, using blocking reads\r\n");
    return 0U;
  }

  if (s_hadc->DMA_Handle) {
    __HAL_DMA_DISABLE_IT(s_hadc->DMA_Handle, DMA_IT_HT);
  }

  s_adc_timed_running = 1U;
  adc_timed_trigger_start(APP_ADC_SAMPLE_INTERVAL_MS);

  print("[ADC] timed sampler started interval=%lu ms channels=%u queue=%u\r\n",
        (unsigned long)APP_ADC_SAMPLE_INTERVAL_MS,
        AI_COUNT,
        APP_ADC_SAMPLE_QUEUE_DEPTH);
  return 1U;
#endif
}

uint8_t app_adc_timed_sampler_is_running(void)
{
  return s_adc_timed_running ? 1U : 0U;
}

uint8_t app_adc_pop_sample_mv(uint32_t out10_mv[10])
{
#if BOARD_AI_SOURCE_SECONDARY_MCU
  secondary_adc_sample_t sample;

  if (!out10_mv) return 0U;
  for (uint8_t i = 0U; i < 10U; i++) out10_mv[i] = 0U;

  if (!secondary_adc_link_pop(&sample)) return 0U;

  if ((sample.channel_count < AI_COUNT) || (sample.status_flags != 0U)) {
    s_adc_dropped_samples++;
    return 0U;
  }

  for (uint8_t i = 0U; i < AI_COUNT && i < 10U; i++) {
    out10_mv[i] = adc_raw_to_scaled_mv(i, sample.raw[i]);
    s_adc_latest_raw[i] = sample.raw[i];
  }
  if (sample.channel_count > AI_COUNT) {
    power_monitor_update(sample.raw[AI_COUNT]);
  }
  s_adc_latest_valid = 1U;
  s_adc_total_samples++;
  return 1U;
#else
  uint16_t raw[AI_COUNT];
  uint32_t primask;

  if (!out10_mv) return 0U;

  for (uint8_t i = 0U; i < 10U; i++) {
    out10_mv[i] = 0U;
  }

  primask = adc_critical_enter();
  if (s_adc_sample_count == 0U) {
    adc_critical_exit(primask);
    return 0U;
  }

  memcpy(raw, s_adc_sample_queue[s_adc_sample_r], sizeof(raw));
  s_adc_sample_r = (uint8_t)((s_adc_sample_r + 1U) % APP_ADC_SAMPLE_QUEUE_DEPTH);
  s_adc_sample_count--;
  adc_critical_exit(primask);

  for (uint8_t i = 0U; i < AI_COUNT && i < 10U; i++) {
    out10_mv[i] = adc_raw_to_scaled_mv(i, raw[i]);
  }

  return 1U;
#endif
}

uint32_t app_adc_timed_sampler_total_samples(void)
{
  return s_adc_total_samples;
}

uint32_t app_adc_timed_sampler_dropped_samples(void)
{
  return s_adc_dropped_samples;
}

uint8_t app_power_monitor_get(uint16_t *raw, uint32_t *sense_mv, uint32_t *supply_mv)
{
  if (!s_power_monitor_valid) return 0U;

  if (raw) *raw = s_power_monitor_raw;
  if (sense_mv) *sense_mv = s_power_monitor_sense_mv;
  if (supply_mv) *supply_mv = s_power_monitor_supply_mv;
  return 1U;
}

void app_adc_dma_conv_cplt_isr(ADC_HandleTypeDef *hadc)
{
  if (!hadc || hadc != s_hadc || !s_adc_timed_running) {
    return;
  }

  memcpy(s_adc_latest_raw, s_adc_dma_raw, sizeof(s_adc_dma_raw));
  s_adc_latest_valid = 1U;

  if (s_adc_sample_count >= APP_ADC_SAMPLE_QUEUE_DEPTH) {
    s_adc_sample_r = (uint8_t)((s_adc_sample_r + 1U) % APP_ADC_SAMPLE_QUEUE_DEPTH);
    s_adc_sample_count--;
    s_adc_dropped_samples++;
  }

  memcpy(s_adc_sample_queue[s_adc_sample_w], s_adc_dma_raw, sizeof(s_adc_dma_raw));
  s_adc_sample_w = (uint8_t)((s_adc_sample_w + 1U) % APP_ADC_SAMPLE_QUEUE_DEPTH);
  s_adc_sample_count++;
  s_adc_total_samples++;
}
/* ---------- public init ---------- */
void app_board_io_init(ADC_HandleTypeDef *hadc)
{
  s_hadc = hadc;

#if !BOARD_AI_SOURCE_SECONDARY_MCU
  ADC1_GPIO_Analog_Init_User();
#endif
  Relay_GPIO_Init_User();
  Relay_Feedback_GPIO_Init_User();
  DI_GPIO_Init_User();
}

static int8_t relay_index_from_key(const char *port_key)
{
    if (!port_key) return -1;

    if (strcmp(port_key, "RO_1") == 0) return 0;
    if (strcmp(port_key, "RO_2") == 0) return 1;
    if (strcmp(port_key, "RO_3") == 0) return 2;
    if (strcmp(port_key, "RO_4") == 0) return 3;
    if (strcmp(port_key, "RO_5") == 0) return 4;

    return -1;
}

/* Relay command from the cloud. Refuses an unknown port and a change of
 * state sooner than APP_RELAY_MIN_SWITCH_MS after the previous change of
 * that relay (chatter protection: a burst test switched a relay 20 times in
 * 2 s). A command that keeps the current state is always accepted.
 * *reason is set on refusal. */
static uint32_t s_relay_last_change_tick[5];
static uint8_t  s_relay_changed_once[5];

uint8_t app_relay_command(const char *port_key, uint8_t on, const char **reason)
{
    int8_t idx = relay_index_from_key(port_key);
    if (idx < 0 || idx >= RELAY_COUNT) {
        if (reason) *reason = "unknown_port";
        return 0U;
    }
    uint8_t current = app_relay_get_by_key(port_key);
    if ((current ? 1U : 0U) != (on ? 1U : 0U)) {
        uint32_t now = HAL_GetTick();
        if (s_relay_changed_once[idx] && (now - s_relay_last_change_tick[idx]) < APP_RELAY_MIN_SWITCH_MS) {
            print("[RELAY][WARN] %s change refused: %lu ms since the last one (min %u)\r\n",
                  port_key, (unsigned long)(now - s_relay_last_change_tick[idx]), (unsigned)APP_RELAY_MIN_SWITCH_MS);
            if (reason) *reason = "min_switch_interval";
            return 0U;
        }
        s_relay_last_change_tick[idx] = now;
        s_relay_changed_once[idx] = 1U;
    }
    if (!app_relay_set_by_key(port_key, on)) {
        if (reason) *reason = "relay_action_failed";
        return 0U;
    }
    return 1U;
}

/* Drive every relay to its configured safe state (bit i set = relay i ON). */
void app_relay_apply_safe_states(uint16_t on_mask, const char *why)
{
    for (uint8_t i = 0; i < RELAY_COUNT && i < 8U; i++) {
        uint8_t on = (uint8_t)((on_mask >> i) & 1U);
        relay_write(i, on);
        print("[RELAY] RO_%u safe state %s (%s)\r\n", (unsigned)(i + 1U), on ? "ON" : "OFF", why ? why : "");
    }
}

uint8_t app_relay_set_by_key(const char *port_key, uint8_t on)
{
    int8_t idx = relay_index_from_key(port_key);

    if (idx < 0 || idx >= RELAY_COUNT) {
        print("[RELAY][ERR] invalid portKey=%s\r\n", port_key ? port_key : "NULL");
        return 0U;
    }

    relay_write((uint8_t)idx, on ? 1U : 0U);

    print("[RELAY] %s %s\r\n",
          port_key,
          on ? "ON" : "OFF");

#if BOARD_RELAY_FEEDBACK_COUNT > 0U
    if ((uint8_t)idx < BOARD_RELAY_FEEDBACK_COUNT) {
        /* The contact needs a few ms to move; reading in the same
         * millisecond as the coil drive always showed the old state. */
        HAL_Delay(RELAY_FEEDBACK_SETTLE_MS);
        print("[RELAY_FB] %s requested=%s feedback=%s after %u ms (ack independent)\r\n",
              port_key,
              on ? "ON" : "OFF",
              relay_feedback_read((uint8_t)idx) ? "ON" : "OFF",
              (unsigned)RELAY_FEEDBACK_SETTLE_MS);
    }
#endif

    return 1U;
}

uint8_t app_relay_feedback_get_by_key(const char *port_key)
{
    int8_t idx = relay_index_from_key(port_key);
    if (idx < 0 || (uint8_t)idx >= BOARD_RELAY_FEEDBACK_COUNT) return 0U;
    return relay_feedback_read((uint8_t)idx);
}

void app_relay_feedback_debug_print(void)
{
#if BOARD_RELAY_FEEDBACK_COUNT > 0U
    for (uint8_t i = 0U; i < BOARD_RELAY_FEEDBACK_COUNT; i++) {
        print("[RELAY_FB] RO_%u command=%s feedback=%s\r\n",
              (unsigned)(i + 1U),
              app_relay_get_by_key((i == 0U) ? "RO_1" : "RO_2") ? "ON" : "OFF",
              relay_feedback_read(i) ? "ON" : "OFF");
    }
#endif
}

uint8_t app_relay_get_by_key(const char *port_key)
{
    int8_t idx = relay_index_from_key(port_key);

    if (idx < 0 || idx >= RELAY_COUNT) {
        return 0U;
    }

#if BOARD_RELAY_ACTIVE_HIGH
    return (HAL_GPIO_ReadPin(g_relays[idx].port, g_relays[idx].pin) == GPIO_PIN_SET) ? 1U : 0U;
#else
    return (HAL_GPIO_ReadPin(g_relays[idx].port, g_relays[idx].pin) == GPIO_PIN_RESET) ? 1U : 0U;
#endif
}

void app_relay_get_all(uint8_t out5[5])
{
    if (!out5) return;

    for (uint8_t i = 0; i < RELAY_COUNT; i++)
    {
#if BOARD_RELAY_ACTIVE_HIGH
        out5[i] = (HAL_GPIO_ReadPin(g_relays[i].port, g_relays[i].pin) == GPIO_PIN_SET) ? 1U : 0U;
#else
        out5[i] = (HAL_GPIO_ReadPin(g_relays[i].port, g_relays[i].pin) == GPIO_PIN_RESET) ? 1U : 0U;
#endif
    }
}
