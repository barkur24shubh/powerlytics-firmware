#include "secondary_adc_link.h"

#include "modbus_crc.h"

#include <string.h>

#define LINK_SOF_0                0xA5U
#define LINK_SOF_1                0x5AU
#define LINK_MESSAGE_RAW_SAMPLES  0x01U
#define LINK_HEADER_SIZE          5U
#define LINK_PAYLOAD_FIXED_SIZE   11U
#define LINK_CRC_SIZE             2U
#define LINK_MAX_FRAME_SIZE       (LINK_HEADER_SIZE + LINK_PAYLOAD_FIXED_SIZE + \
                                   (SECONDARY_ADC_MAX_CHANNELS * 2U) + LINK_CRC_SIZE)
#define LINK_QUEUE_DEPTH          64U
#define LINK_RX_CHUNK_SIZE        64U

static UART_HandleTypeDef *s_uart;
static uint8_t s_rx_chunk[LINK_RX_CHUNK_SIZE];
static uint8_t s_frame[LINK_MAX_FRAME_SIZE];
static volatile uint8_t s_frame_length;
static volatile uint8_t s_expected_length;
static secondary_adc_sample_t s_queue[LINK_QUEUE_DEPTH];
static volatile uint8_t s_queue_read;
static volatile uint8_t s_queue_write;
static volatile uint8_t s_queue_count;
static secondary_adc_link_stats_t s_stats;
static uint32_t s_last_sequence;
static uint8_t s_sequence_seeded;

static uint16_t read_u16_le(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *data)
{
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

static void reset_parser(void)
{
  s_frame_length = 0U;
  s_expected_length = 0U;
}

static void queue_sample(const secondary_adc_sample_t *sample)
{
  if (s_queue_count >= LINK_QUEUE_DEPTH) {
    s_queue_read = (uint8_t)((s_queue_read + 1U) % LINK_QUEUE_DEPTH);
    s_queue_count--;
    s_stats.queue_overflows++;
  }

  s_queue[s_queue_write] = *sample;
  s_queue_write = (uint8_t)((s_queue_write + 1U) % LINK_QUEUE_DEPTH);
  s_queue_count++;
}

static void process_frame(void)
{
  secondary_adc_sample_t sample = {0};
  const uint8_t payload_length = s_frame[4];
  const uint8_t channel_count = s_frame[13];
  const uint8_t expected_payload = (uint8_t)(LINK_PAYLOAD_FIXED_SIZE + (channel_count * 2U));
  const uint16_t received_crc = read_u16_le(&s_frame[s_frame_length - LINK_CRC_SIZE]);
  const uint16_t calculated_crc = mb_crc16(&s_frame[2],
                                            (uint16_t)(s_frame_length - 2U - LINK_CRC_SIZE));

  if ((s_frame[2] != SECONDARY_ADC_PROTOCOL_VERSION) ||
      (s_frame[3] != LINK_MESSAGE_RAW_SAMPLES) ||
      (channel_count == 0U) ||
      (channel_count > SECONDARY_ADC_MAX_CHANNELS) ||
      (payload_length != expected_payload)) {
    s_stats.format_errors++;
    return;
  }

  if (received_crc != calculated_crc) {
    s_stats.crc_errors++;
    return;
  }

  sample.sequence = read_u32_le(&s_frame[5]);
  sample.source_tick_ms = read_u32_le(&s_frame[9]);
  sample.channel_count = channel_count;
  for (uint8_t i = 0U; i < channel_count; i++) {
    sample.raw[i] = read_u16_le(&s_frame[14U + (i * 2U)]);
  }
  sample.status_flags = read_u16_le(&s_frame[14U + (channel_count * 2U)]);

  if (s_sequence_seeded && sample.sequence != (s_last_sequence + 1U)) {
    if (sample.sequence > (s_last_sequence + 1U)) {
      s_stats.sequence_gaps += sample.sequence - (s_last_sequence + 1U);
    } else {
      s_stats.sequence_gaps++;
    }
  }
  s_last_sequence = sample.sequence;
  s_sequence_seeded = 1U;
  s_stats.valid_frames++;
  queue_sample(&sample);
}

static void consume_byte(uint8_t value)
{
  if (s_frame_length == 0U) {
    if (value == LINK_SOF_0) s_frame[s_frame_length++] = value;
    return;
  }

  if (s_frame_length == 1U) {
    if (value == LINK_SOF_1) {
      s_frame[s_frame_length++] = value;
    } else {
      reset_parser();
      if (value == LINK_SOF_0) s_frame[s_frame_length++] = value;
    }
    return;
  }

  if (s_frame_length >= LINK_MAX_FRAME_SIZE) {
    s_stats.format_errors++;
    reset_parser();
    return;
  }

  s_frame[s_frame_length++] = value;
  if (s_frame_length == LINK_HEADER_SIZE) {
    const uint16_t total = LINK_HEADER_SIZE + s_frame[4] + LINK_CRC_SIZE;
    if (total > LINK_MAX_FRAME_SIZE || s_frame[4] < LINK_PAYLOAD_FIXED_SIZE) {
      s_stats.format_errors++;
      reset_parser();
      return;
    }
    s_expected_length = (uint8_t)total;
  }

  if (s_expected_length != 0U && s_frame_length == s_expected_length) {
    process_frame();
    reset_parser();
  }
}

uint8_t secondary_adc_link_start(UART_HandleTypeDef *uart)
{
  if (!uart) return 0U;

  s_uart = uart;
  reset_parser();
  s_queue_read = 0U;
  s_queue_write = 0U;
  s_queue_count = 0U;
  s_sequence_seeded = 0U;
  memset(&s_stats, 0, sizeof(s_stats));

  return (HAL_UARTEx_ReceiveToIdle_IT(s_uart,
                                     s_rx_chunk,
                                     sizeof(s_rx_chunk)) == HAL_OK) ? 1U : 0U;
}

uint8_t secondary_adc_link_pop(secondary_adc_sample_t *sample)
{
  uint32_t primask;

  if (!sample) return 0U;
  primask = __get_PRIMASK();
  __disable_irq();
  if (s_queue_count == 0U) {
    if (primask == 0U) __enable_irq();
    return 0U;
  }
  *sample = s_queue[s_queue_read];
  s_queue_read = (uint8_t)((s_queue_read + 1U) % LINK_QUEUE_DEPTH);
  s_queue_count--;
  if (primask == 0U) __enable_irq();
  return 1U;
}

void secondary_adc_link_get_stats(secondary_adc_link_stats_t *stats)
{
  uint32_t primask;
  if (!stats) return;
  primask = __get_PRIMASK();
  __disable_irq();
  *stats = s_stats;
  if (primask == 0U) __enable_irq();
}

void secondary_adc_link_uart_rx_complete_isr(UART_HandleTypeDef *uart)
{
  (void)uart;
}

void secondary_adc_link_uart_rx_event_isr(UART_HandleTypeDef *uart, uint16_t size)
{
  if (!uart || uart != s_uart) return;

  if (size > sizeof(s_rx_chunk)) size = sizeof(s_rx_chunk);
  for (uint16_t i = 0U; i < size; i++) {
    consume_byte(s_rx_chunk[i]);
  }

  (void)HAL_UARTEx_ReceiveToIdle_IT(s_uart,
                                   s_rx_chunk,
                                   sizeof(s_rx_chunk));
}

void secondary_adc_link_uart_error_isr(UART_HandleTypeDef *uart)
{
  if (!uart || uart != s_uart) return;
  s_stats.uart_errors++;
  reset_parser();
  (void)HAL_UART_AbortReceive(s_uart);
  __HAL_UART_CLEAR_OREFLAG(s_uart);
  (void)HAL_UARTEx_ReceiveToIdle_IT(s_uart,
                                   s_rx_chunk,
                                   sizeof(s_rx_chunk));
}
