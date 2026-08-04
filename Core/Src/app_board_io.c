#include "app_board_io.h"

#include "stm32f4xx_hal.h"
#include "debug_uart.h"

#include <stdint.h>
#include <string.h>

/* Same PinDef as your main */
typedef struct {
  GPIO_TypeDef *port;
  uint16_t pin;
} PinDef;

/* =========================================================
 * Relay pins from schematic
 * =========================================================
 * DOUT4 -> PB11
 * DOUT3 -> PB12
 * DOUT2 -> PB13
 * DOUT1 -> PB14
 * DOUT0 -> PB15
 *
 * Relay logical mapping used by firmware:
 * R1 -> DOUT0 -> PB15
 * R2 -> DOUT1 -> PB14
 * R3 -> DOUT2 -> PB13
 * R4 -> DOUT3 -> PB12
 * R5 -> DOUT4 -> PB11
 *
 * IMPORTANT:
 * Do not use PC6/PC7 for relay. USART6 modem uses PC6/PC7.
 */
#define RELAY_ACTIVE_HIGH  1U

static const PinDef g_relays[] = {
  {GPIOB, GPIO_PIN_15},  /* R1 / DOUT0 */
  {GPIOB, GPIO_PIN_14},  /* R2 / DOUT1 */
  {GPIOB, GPIO_PIN_13},  /* R3 / DOUT2 */
  {GPIOB, GPIO_PIN_12},  /* R4 / DOUT3 */
  {GPIOB, GPIO_PIN_11},  /* R5 / DOUT4 */
};

#define RELAY_COUNT ((uint8_t)(sizeof(g_relays) / sizeof(g_relays[0])))

/* Digital input pins */
static const PinDef g_dis[] = {
  {GPIOE, GPIO_PIN_8},   // DI_1
  {GPIOE, GPIO_PIN_9},   // DI_2
  {GPIOE, GPIO_PIN_10},  // DI_3
  {GPIOE, GPIO_PIN_11},  // DI_4
  {GPIOE, GPIO_PIN_12},  // DI_5
  {GPIOE, GPIO_PIN_13},  // DI_6
  {GPIOE, GPIO_PIN_14},  // DI_7
  {GPIOE, GPIO_PIN_15},  // DI_8
};

#define DI_COUNT ((uint8_t)(sizeof(g_dis) / sizeof(g_dis[0])))

#define ADC_MAX_COUNTS 4095U
#define ADC_VREF_MV    3320U
#define APP_ADC_DEBUG_PRINT 0U

static ADC_HandleTypeDef *s_hadc = NULL;

/* ---------- RELAY ---------- */
static void relay_write_raw(uint8_t relay_index, uint8_t on)
{
  if (relay_index >= RELAY_COUNT) {
    return;
  }

#if RELAY_ACTIVE_HIGH
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

#if RELAY_ACTIVE_HIGH
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

  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* Keep all relays OFF before switching pins to output mode. */
#if RELAY_ACTIVE_HIGH
  HAL_GPIO_WritePin(GPIOB,
                    GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15,
                    GPIO_PIN_RESET);
#else
  HAL_GPIO_WritePin(GPIOB,
                    GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15,
                    GPIO_PIN_SET);
#endif

  GPIO_InitStruct.Pin   = GPIO_PIN_11 |
                          GPIO_PIN_12 |
                          GPIO_PIN_13 |
                          GPIO_PIN_14 |
                          GPIO_PIN_15;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    relay_write_raw(i, 0U);
  }

  print("[RELAY] init done count=%u pins=PB15,PB14,PB13,PB12,PB11\r\n", RELAY_COUNT);
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

  __HAL_RCC_GPIOE_CLK_ENABLE();

  GPIO_InitStruct.Pin =
      GPIO_PIN_8  |
      GPIO_PIN_9  |
      GPIO_PIN_10 |
      GPIO_PIN_11 |
      GPIO_PIN_12 |
      GPIO_PIN_13 |
      GPIO_PIN_14 |
      GPIO_PIN_15;

  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;

  /*
   * Keep this as PULLDOWN if your digital input circuit drives HIGH when active.
   * If your optocoupler/input circuit pulls the pin LOW when active, use GPIO_PULLUP instead.
   */
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;

  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);
}

static uint8_t di_read(GPIO_TypeDef *port, uint16_t pin)
{
  return (HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_SET) ? 1U : 0U;
}

void app_di_read_all(uint8_t out8[8])
{
  if (!out8) return;

  for (int i = 0; i < 8; i++) {
    out8[i] = 0U;
  }

  for (int i = 0; i < (int)DI_COUNT; i++) {
    out8[i] = di_read(g_dis[i].port, g_dis[i].pin);
  }
}

/* ---------- ADC ---------- */
static void ADC1_GPIO_Analog_Init_User(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;

  /*
   * PC0 -> ADC_CHANNEL_10
   * PC1 -> ADC_CHANNEL_11
   * PC2 -> ADC_CHANNEL_12
   * PC3 -> ADC_CHANNEL_13
   */
  GPIO_InitStruct.Pin = GPIO_PIN_0 |
                        GPIO_PIN_1 |
                        GPIO_PIN_2 |
                        GPIO_PIN_3;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*
   * PA0 -> ADC_CHANNEL_0
   * PA1 -> ADC_CHANNEL_1
   * PA2 -> ADC_CHANNEL_2
   * PA3 -> ADC_CHANNEL_3
   * PA4 -> ADC_CHANNEL_4
   * PA5 -> ADC_CHANNEL_5
   */
  GPIO_InitStruct.Pin = GPIO_PIN_0 |
                        GPIO_PIN_1 |
                        GPIO_PIN_2 |
                        GPIO_PIN_3 |
                        GPIO_PIN_4 |
                        GPIO_PIN_5;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
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

//void app_adc_read_all_mv(uint32_t out10_mv[10])
//{
//  if (!out10_mv) return;
//
//  uint16_t a0 = adc1_read_avg(ADC_CHANNEL_10,16); // PC0
//  uint16_t a1 = adc1_read_avg(ADC_CHANNEL_11,16); // PC1
//  uint16_t a2 = adc1_read_avg(ADC_CHANNEL_12,16); // PC2
//  uint16_t a3 = adc1_read_avg(ADC_CHANNEL_13,16); // PC3
//
//  uint16_t a4 = adc1_read_avg(ADC_CHANNEL_0,16);  // PA0
//  uint16_t a5 = adc1_read_avg(ADC_CHANNEL_1,16);  // PA1
//  uint16_t a6 = adc1_read_avg(ADC_CHANNEL_2,16);  // PA2
//  uint16_t a7 = adc1_read_avg(ADC_CHANNEL_3,16);  // PA3
//  uint16_t a8 = adc1_read_avg(ADC_CHANNEL_4,16);  // PA4
//  uint16_t a9 = adc1_read_avg(ADC_CHANNEL_5,16);  // PA5
//
//  out10_mv[0] = adc_to_mV(a0);
//  out10_mv[1] = adc_to_mV(a1);
//  out10_mv[2] = (uint32_t)((float)adc_to_mV(a2) * 3.94f);
//  out10_mv[3] = (uint32_t)((float)adc_to_mV(a3) * 3.94f);
//  out10_mv[4] = (uint32_t)((float)adc_to_mV(a4) * 3.94f);
//  out10_mv[5] = (uint32_t)((float)adc_to_mV(a5) * 3.94f);
//  out10_mv[6] = (uint32_t)((float)adc_to_mV(a6) * 3.94f);
//  out10_mv[7] = (uint32_t)((float)adc_to_mV(a7) * 3.94f);
//  out10_mv[8] = (uint32_t)((float)adc_to_mV(a8) * 3.94f);
//  out10_mv[9] = (uint32_t)((float)adc_to_mV(a9) * 3.94f);
//}


void app_adc_read_all_mv(uint32_t out10_mv[10])
{
  if (!out10_mv) return;

  uint16_t raw[10] = {0};

  raw[0] = adc1_read_avg(ADC_CHANNEL_10, 16); // AI_1  PC0
  raw[1] = adc1_read_avg(ADC_CHANNEL_11, 16); // AI_2  PC1
  raw[2] = adc1_read_avg(ADC_CHANNEL_12, 16); // AI_3  PC2
  raw[3] = adc1_read_avg(ADC_CHANNEL_13, 16); // AI_4  PC3

  raw[4] = adc1_read_avg(ADC_CHANNEL_0, 16);  // AI_5  PA0
  raw[5] = adc1_read_avg(ADC_CHANNEL_1, 16);  // AI_6  PA1
  raw[6] = adc1_read_avg(ADC_CHANNEL_2, 16);  // AI_7  PA2
  raw[7] = adc1_read_avg(ADC_CHANNEL_3, 16);  // AI_8  PA3
  raw[8] = adc1_read_avg(ADC_CHANNEL_4, 16);  // AI_9  PA4
  raw[9] = adc1_read_avg(ADC_CHANNEL_5, 16);  // AI_10 PA5

  for (int i = 0; i < 10; i++)
  {
    uint32_t pin_mv = adc_to_mV(raw[i]);
    uint32_t final_mv;

    if (i < 2) {
      final_mv = pin_mv;
    } else {
      final_mv = (uint32_t)((float)pin_mv * 3.94f);
    }

    out10_mv[i] = final_mv;

#if APP_ADC_DEBUG_PRINT
    const char *pin_name[10] = {
      "PC0", "PC1", "PC2", "PC3", "PA0",
      "PA1", "PA2", "PA3", "PA4", "PA5"
    };

    const char *scale_name[10] = {
      "1.00", "1.00", "3.94", "3.94", "3.94",
      "3.94", "3.94", "3.94", "3.94", "3.94"
    };

    print("[ADC] AI_%d pin=%s raw=%u pin_mv=%lu final_mv=%lu scale=%s\r\n",
          i + 1,
          pin_name[i],
          (unsigned)raw[i],
          (unsigned long)pin_mv,
          (unsigned long)final_mv,
          scale_name[i]);
#endif
  }
}
/* ---------- public init ---------- */
void app_board_io_init(ADC_HandleTypeDef *hadc)
{
  s_hadc = hadc;

  ADC1_GPIO_Analog_Init_User();
  Relay_GPIO_Init_User();
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

    return 1U;
}

uint8_t app_relay_get_by_key(const char *port_key)
{
    int8_t idx = relay_index_from_key(port_key);

    if (idx < 0 || idx >= RELAY_COUNT) {
        return 0U;
    }

#if RELAY_ACTIVE_HIGH
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
#if RELAY_ACTIVE_HIGH
        out5[i] = (HAL_GPIO_ReadPin(g_relays[i].port, g_relays[i].pin) == GPIO_PIN_SET) ? 1U : 0U;
#else
        out5[i] = (HAL_GPIO_ReadPin(g_relays[i].port, g_relays[i].pin) == GPIO_PIN_RESET) ? 1U : 0U;
#endif
    }
}
