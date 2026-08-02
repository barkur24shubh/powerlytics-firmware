#include "modbus_rtu_master.h"
#include "modbus_crc.h"
#include <string.h>

/* Optional tiny settle after switching to RX (helps some transceivers) */
static void rs485_settle_delay(void)
{
  for (volatile int i = 0; i < 2000; i++) { __NOP(); }
}

#define MBM_TX_COMPLETE_TIMEOUT_MS 100U

void mbm_init(mbm_t *m,
              UART_HandleTypeDef *huart,
              rs485_t *rs,
              uint8_t *rx_buf,
              uint16_t rx_buf_size,
              osSemaphoreId_t rx_sem)
{
  m->huart = huart;
  m->rs485 = rs;
  m->rx_buf = rx_buf;
  m->rx_buf_size = rx_buf_size;
  m->rx_len = 0;
  m->rx_sem = rx_sem;
}

HAL_StatusTypeDef mbm_arm_rx_to_idle(mbm_t *m)
{
  m->rx_len = 0;

  HAL_UART_AbortReceive(m->huart);
  __HAL_UART_CLEAR_IDLEFLAG(m->huart);

  HAL_StatusTypeDef st = HAL_UARTEx_ReceiveToIdle_DMA(m->huart, m->rx_buf, m->rx_buf_size);
  if (m->huart->hdmarx) __HAL_DMA_DISABLE_IT(m->huart->hdmarx, DMA_IT_HT);
  return st;
}

void mbm_on_rx_event(mbm_t *m, UART_HandleTypeDef *huart, uint16_t size)
{
  if (huart != m->huart) return;
  if (size == 0) return;            // ignore spurious idle
  m->rx_len = size;
  osSemaphoreRelease(m->rx_sem);
}

HAL_StatusTypeDef mbm_configure_serial(mbm_t *m,
                                       uint32_t baud,
                                       uint8_t data_bits,
                                       uint8_t stop_bits,
                                       uint8_t parity)
{
  if (!m || !m->huart || data_bits != 8U) {
    return HAL_ERROR;
  }

  if (baud == 0U) {
    baud = 9600U;
  }

  uint32_t word_length = UART_WORDLENGTH_8B;
  uint32_t parity_cfg = UART_PARITY_NONE;
  uint32_t stop_bits_cfg = (stop_bits == 2U) ? UART_STOPBITS_2 : UART_STOPBITS_1;

  if (parity == 1U) {
    parity_cfg = UART_PARITY_EVEN;
    word_length = UART_WORDLENGTH_9B;
  } else if (parity == 2U) {
    parity_cfg = UART_PARITY_ODD;
    word_length = UART_WORDLENGTH_9B;
  }

  if (m->huart->Init.BaudRate == baud &&
      m->huart->Init.WordLength == word_length &&
      m->huart->Init.StopBits == stop_bits_cfg &&
      m->huart->Init.Parity == parity_cfg) {
    return HAL_OK;
  }

  if (m->rs485) {
    rs485_rx(m->rs485);
  }

  (void)HAL_UART_AbortReceive(m->huart);
  (void)HAL_UART_AbortTransmit(m->huart);

  if (m->rx_sem) {
    while (osSemaphoreAcquire(m->rx_sem, 0U) == osOK) {
      /* drain stale frame notifications from previous UART settings */
    }
  }

  m->rx_len = 0U;

  if (HAL_UART_DeInit(m->huart) != HAL_OK) {
    return HAL_ERROR;
  }

  m->huart->Init.BaudRate = baud;
  m->huart->Init.WordLength = word_length;
  m->huart->Init.StopBits = stop_bits_cfg;
  m->huart->Init.Parity = parity_cfg;
  m->huart->Init.Mode = UART_MODE_TX_RX;
  m->huart->Init.HwFlowCtl = UART_HWCONTROL_NONE;
  m->huart->Init.OverSampling = UART_OVERSAMPLING_16;

  if (HAL_UART_Init(m->huart) != HAL_OK) {
    return HAL_ERROR;
  }

  __HAL_UART_CLEAR_OREFLAG(m->huart);
  __HAL_UART_CLEAR_NEFLAG(m->huart);
  __HAL_UART_CLEAR_FEFLAG(m->huart);
  __HAL_UART_CLEAR_PEFLAG(m->huart);
  __HAL_UART_CLEAR_IDLEFLAG(m->huart);

  return HAL_OK;
}

static HAL_StatusTypeDef mbm_tx_frame(mbm_t *m, const uint8_t *buf, uint16_t len)
{
  rs485_tx(m->rs485);

  HAL_StatusTypeDef st = HAL_UART_Transmit(m->huart, (uint8_t*)buf, len, 200);
  if (st != HAL_OK) {
    rs485_rx(m->rs485);
    return st;
  }

  uint32_t tc_start = HAL_GetTick();
  while (__HAL_UART_GET_FLAG(m->huart, UART_FLAG_TC) == RESET) {
    if ((HAL_GetTick() - tc_start) > MBM_TX_COMPLETE_TIMEOUT_MS) {
      rs485_rx(m->rs485);
      return HAL_TIMEOUT;
    }
  }

  rs485_rx(m->rs485);
  rs485_settle_delay();
  return HAL_OK;
}

HAL_StatusTypeDef mbm_read_holding_03(mbm_t *m,
                                     uint8_t slave_id,
                                     uint16_t start_reg,
                                     uint16_t qty,
                                     uint32_t timeout_ms,
                                     uint16_t *out_regs)
{
  if (qty < 1 || qty > 125) return HAL_ERROR;

  uint8_t req[8];
  req[0] = slave_id;
  req[1] = 0x03;
  req[2] = (uint8_t)(start_reg >> 8);
  req[3] = (uint8_t)(start_reg & 0xFF);
  req[4] = (uint8_t)(qty >> 8);
  req[5] = (uint8_t)(qty & 0xFF);

  uint16_t crc = mb_crc16(req, 6);
  req[6] = (uint8_t)(crc & 0xFF);
  req[7] = (uint8_t)(crc >> 8);

  /* Arm RX before TX */
  if (mbm_arm_rx_to_idle(m) != HAL_OK) return HAL_ERROR;

  if (mbm_tx_frame(m, req, sizeof(req)) != HAL_OK) return HAL_ERROR;

  if (osSemaphoreAcquire(m->rx_sem, timeout_ms) != osOK) return HAL_TIMEOUT;

  uint16_t n = m->rx_len;
  if (n < 5) return HAL_ERROR;

  if (m->rx_buf[0] != slave_id) return HAL_ERROR;

  uint8_t fc = m->rx_buf[1];
  if (fc & 0x80) return HAL_ERROR;
  if (fc != 0x03) return HAL_ERROR;

  uint8_t bc = m->rx_buf[2];
  uint16_t frame_len = (uint16_t)(3 + bc + 2);
  if (n < frame_len) return HAL_ERROR;

  uint16_t rx_crc = (uint16_t)m->rx_buf[frame_len-2] | ((uint16_t)m->rx_buf[frame_len-1] << 8);
  uint16_t calc   = mb_crc16(m->rx_buf, frame_len-2);
  if (rx_crc != calc) return HAL_ERROR;

  if (bc != qty * 2) return HAL_ERROR;

  for (uint16_t i = 0; i < qty; i++) {
    uint16_t hi = m->rx_buf[3 + (i*2)];
    uint16_t lo = m->rx_buf[4 + (i*2)];
    out_regs[i] = (uint16_t)((hi << 8) | lo);
  }

  return HAL_OK;
}

HAL_StatusTypeDef mbm_read_input_04(mbm_t *m,
                                   uint8_t slave_id,
                                   uint16_t start_reg,
                                   uint16_t qty,
                                   uint32_t timeout_ms,
                                   uint16_t *out_regs)
{
  if (qty < 1 || qty > 125) return HAL_ERROR;

  uint8_t req[8];
  req[0] = slave_id;
  req[1] = 0x04;
  req[2] = (uint8_t)(start_reg >> 8);
  req[3] = (uint8_t)(start_reg & 0xFF);
  req[4] = (uint8_t)(qty >> 8);
  req[5] = (uint8_t)(qty & 0xFF);

  uint16_t crc = mb_crc16(req, 6);
  req[6] = (uint8_t)(crc & 0xFF);
  req[7] = (uint8_t)(crc >> 8);

  /* Arm RX before TX */
  if (mbm_arm_rx_to_idle(m) != HAL_OK) return HAL_ERROR;

  if (mbm_tx_frame(m, req, sizeof(req)) != HAL_OK) return HAL_ERROR;

  if (osSemaphoreAcquire(m->rx_sem, timeout_ms) != osOK) return HAL_TIMEOUT;

  uint16_t n = m->rx_len;
  if (n < 5) return HAL_ERROR;

  if (m->rx_buf[0] != slave_id) return HAL_ERROR;

  uint8_t fc = m->rx_buf[1];
  if (fc & 0x80) return HAL_ERROR;
  if (fc != 0x04) return HAL_ERROR;

  uint8_t bc = m->rx_buf[2];
  uint16_t frame_len = (uint16_t)(3 + bc + 2);
  if (n < frame_len) return HAL_ERROR;

  uint16_t rx_crc = (uint16_t)m->rx_buf[frame_len-2] | ((uint16_t)m->rx_buf[frame_len-1] << 8);
  uint16_t calc   = mb_crc16(m->rx_buf, frame_len-2);
  if (rx_crc != calc) return HAL_ERROR;

  if (bc != qty * 2) return HAL_ERROR;

  for (uint16_t i = 0; i < qty; i++) {
    uint16_t hi = m->rx_buf[3 + (i*2)];
    uint16_t lo = m->rx_buf[4 + (i*2)];
    out_regs[i] = (uint16_t)((hi << 8) | lo);
  }

  return HAL_OK;
}
