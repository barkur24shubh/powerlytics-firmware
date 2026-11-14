/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file         stm32f4xx_hal_msp.c
  * @brief        This file provides code for the MSP Initialization
  *               and de-Initialization codes.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
/* USER CODE BEGIN Includes */
#include "board_profile.h"

/* USER CODE END Includes */
extern DMA_HandleTypeDef hdma_usart3_rx;

extern DMA_HandleTypeDef hdma_adc1;

extern DMA_HandleTypeDef hdma_usart6_rx;

extern DMA_HandleTypeDef hdma_usart6_tx;

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN Define */

/* USER CODE END Define */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN Macro */

/* USER CODE END Macro */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* External functions --------------------------------------------------------*/
/* USER CODE BEGIN ExternalFunctions */

/* USER CODE END ExternalFunctions */

/* USER CODE BEGIN 0 */
static void board_msp_gpio_enable_clock(GPIO_TypeDef *port)
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

static void board_msp_uart_gpio_init(GPIO_TypeDef *tx_port,
                                     uint16_t tx_pin,
                                     GPIO_TypeDef *rx_port,
                                     uint16_t rx_pin,
                                     uint32_t alternate)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  board_msp_gpio_enable_clock(tx_port);
  board_msp_gpio_enable_clock(rx_port);

  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = alternate;

  if (tx_port == rx_port) {
    GPIO_InitStruct.Pin = tx_pin | rx_pin;
    HAL_GPIO_Init(tx_port, &GPIO_InitStruct);
  } else {
    GPIO_InitStruct.Pin = tx_pin;
    HAL_GPIO_Init(tx_port, &GPIO_InitStruct);
    GPIO_InitStruct.Pin = rx_pin;
    HAL_GPIO_Init(rx_port, &GPIO_InitStruct);
  }
}

static void board_msp_uart_gpio_deinit(GPIO_TypeDef *tx_port,
                                       uint16_t tx_pin,
                                       GPIO_TypeDef *rx_port,
                                       uint16_t rx_pin)
{
  if (tx_port == rx_port) {
    HAL_GPIO_DeInit(tx_port, tx_pin | rx_pin);
  } else {
    HAL_GPIO_DeInit(tx_port, tx_pin);
    HAL_GPIO_DeInit(rx_port, rx_pin);
  }
}

/* USER CODE END 0 */
/**
  * Initializes the Global MSP.
  */
void HAL_MspInit(void)
{

  /* USER CODE BEGIN MspInit 0 */

  /* USER CODE END MspInit 0 */

  __HAL_RCC_SYSCFG_CLK_ENABLE();
  __HAL_RCC_PWR_CLK_ENABLE();

  /* System interrupt init*/
  /* PendSV_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(PendSV_IRQn, 15, 0);

  /* USER CODE BEGIN MspInit 1 */

  /* USER CODE END MspInit 1 */
}

/**
  * @brief ADC MSP Initialization
  * This function configures the hardware resources used in this example
  * @param hadc: ADC handle pointer
  * @retval None
  */
void HAL_ADC_MspInit(ADC_HandleTypeDef* hadc)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  static const board_adc_channel_t ai_channels[] = BOARD_AI_CHANNELS_INITIALIZER;

  if(hadc->Instance == ADC1)
  {
    /* Peripheral clock enable */
    __HAL_RCC_ADC1_CLK_ENABLE();

    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = GPIO_NOPULL;

    for (uint8_t i = 0U;
         i < (uint8_t)(sizeof(ai_channels) / sizeof(ai_channels[0]));
         i++) {
      board_msp_gpio_enable_clock(ai_channels[i].port);
      GPIO_InitStruct.Pin = ai_channels[i].pin;
      HAL_GPIO_Init(ai_channels[i].port, &GPIO_InitStruct);
    }

    hdma_adc1.Instance = DMA2_Stream0;
    hdma_adc1.Init.Channel = DMA_CHANNEL_0;
    hdma_adc1.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_adc1.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_adc1.Init.MemInc = DMA_MINC_ENABLE;
    hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_adc1.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma_adc1.Init.Mode = DMA_CIRCULAR;
    hdma_adc1.Init.Priority = DMA_PRIORITY_HIGH;
    hdma_adc1.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_adc1) != HAL_OK)
    {
      Error_Handler();
    }

    __HAL_LINKDMA(hadc, DMA_Handle, hdma_adc1);
  }
}

/**
  * @brief ADC MSP De-Initialization
  * This function freeze the hardware resources used in this example
  * @param hadc: ADC handle pointer
  * @retval None
  */
void HAL_ADC_MspDeInit(ADC_HandleTypeDef* hadc)
{
  if(hadc->Instance == ADC1)
  {
    static const board_adc_channel_t ai_channels[] = BOARD_AI_CHANNELS_INITIALIZER;

    /* Peripheral clock disable */
    __HAL_RCC_ADC1_CLK_DISABLE();

    for (uint8_t i = 0U;
         i < (uint8_t)(sizeof(ai_channels) / sizeof(ai_channels[0]));
         i++) {
      HAL_GPIO_DeInit(ai_channels[i].port, ai_channels[i].pin);
    }

    HAL_DMA_DeInit(hadc->DMA_Handle);
  }
}

/**
  * @brief RTC MSP Initialization
  * This function configures the hardware resources used in this example
  * @param hrtc: RTC handle pointer
  * @retval None
  */
void HAL_RTC_MspInit(RTC_HandleTypeDef* hrtc)
{
  if(hrtc->Instance == RTC)
  {
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    /*
     * Do not select RTC clock here.
     * rtc_manager.c selects LSE first, then LSI fallback.
     */
    __HAL_RCC_RTC_ENABLE();
  }
}

/**
  * @brief RTC MSP De-Initialization
  * This function freeze the hardware resources used in this example
  * @param hrtc: RTC handle pointer
  * @retval None
  */
void HAL_RTC_MspDeInit(RTC_HandleTypeDef* hrtc)
{
  if(hrtc->Instance == RTC)
  {
    /*
     * Usually do not disable RTC in production.
     * But keeping this for HAL completeness.
     */
    __HAL_RCC_RTC_DISABLE();
  }
}

/**
  * @brief SD MSP Initialization
  * @param hsd: SD handle pointer
  * @retval None
  */
void HAL_SD_MspInit(SD_HandleTypeDef* hsd)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  if(hsd->Instance==SDIO)
  {
    __HAL_RCC_SDIO_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    GPIO_InitStruct.Pin = BOARD_SDIO_GPIOC_PINS;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF12_SDIO;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = BOARD_SDIO_GPIOD_PINS;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF12_SDIO;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);
  }
}

/**
  * @brief SD MSP De-Initialization
  * @param hsd: SD handle pointer
  * @retval None
  */
void HAL_SD_MspDeInit(SD_HandleTypeDef* hsd)
{
  if(hsd->Instance==SDIO)
  {
    __HAL_RCC_SDIO_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOC, BOARD_SDIO_GPIOC_PINS);
    HAL_GPIO_DeInit(GPIOD, BOARD_SDIO_GPIOD_PINS);
  }
}

/**
  * @brief SPI MSP Initialization
  * This function configures the hardware resources used in this example
  * @param hspi: SPI handle pointer
  * @retval None
  */
void HAL_SPI_MspInit(SPI_HandleTypeDef* hspi)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(hspi->Instance==BOARD_W25Q_SPI_INSTANCE)
  {
    /* USER CODE BEGIN SPI1_MspInit 0 */

    /* USER CODE END SPI1_MspInit 0 */
    /* Peripheral clock enable */
    __HAL_RCC_SPI1_CLK_ENABLE();

    board_msp_gpio_enable_clock(BOARD_W25Q_SPI_GPIO_PORT);
    GPIO_InitStruct.Pin = BOARD_W25Q_SPI_SCK_GPIO_PIN |
                          BOARD_W25Q_SPI_MISO_GPIO_PIN |
                          BOARD_W25Q_SPI_MOSI_GPIO_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(BOARD_W25Q_SPI_GPIO_PORT, &GPIO_InitStruct);

    /* USER CODE BEGIN SPI1_MspInit 1 */

    /* USER CODE END SPI1_MspInit 1 */

  }

}

/**
  * @brief SPI MSP De-Initialization
  * This function freeze the hardware resources used in this example
  * @param hspi: SPI handle pointer
  * @retval None
  */
void HAL_SPI_MspDeInit(SPI_HandleTypeDef* hspi)
{
  if(hspi->Instance==BOARD_W25Q_SPI_INSTANCE)
  {
    /* USER CODE BEGIN SPI1_MspDeInit 0 */

    /* USER CODE END SPI1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_SPI1_CLK_DISABLE();

    /**SPI1 GPIO Configuration
    PB3     ------> SPI1_SCK
    PB4     ------> SPI1_MISO
    PB5     ------> SPI1_MOSI
    */
    HAL_GPIO_DeInit(BOARD_W25Q_SPI_GPIO_PORT,
                    BOARD_W25Q_SPI_SCK_GPIO_PIN |
                    BOARD_W25Q_SPI_MISO_GPIO_PIN |
                    BOARD_W25Q_SPI_MOSI_GPIO_PIN);

    /* USER CODE BEGIN SPI1_MspDeInit 1 */

    /* USER CODE END SPI1_MspDeInit 1 */
  }

}

/**
  * @brief UART MSP Initialization
  * This function configures the hardware resources used in this example
  * @param huart: UART handle pointer
  * @retval None
  */
void HAL_UART_MspInit(UART_HandleTypeDef* huart)
{
  if(huart->Instance==USART1)
  {
    /* USER CODE BEGIN USART1_MspInit 0 */
    /* USER CODE END USART1_MspInit 0 */
    /* Peripheral clock enable */
    __HAL_RCC_USART1_CLK_ENABLE();

    board_msp_uart_gpio_init(BOARD_SECONDARY_ADC_UART_TX_GPIO_PORT,
                             BOARD_SECONDARY_ADC_UART_TX_GPIO_PIN,
                             BOARD_SECONDARY_ADC_UART_RX_GPIO_PORT,
                             BOARD_SECONDARY_ADC_UART_RX_GPIO_PIN,
                             GPIO_AF7_USART1);

    HAL_NVIC_SetPriority(USART1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);

    /* USER CODE BEGIN USART1_MspInit 1 */
    /* USER CODE END USART1_MspInit 1 */
  }
  else if(huart->Instance==USART3)
  {
    /* USER CODE BEGIN USART3_MspInit 0 */

    /* USER CODE END USART3_MspInit 0 */
    /* Peripheral clock enable */
    __HAL_RCC_USART3_CLK_ENABLE();

    board_msp_uart_gpio_init(BOARD_MODBUS_UART_TX_GPIO_PORT,
                             BOARD_MODBUS_UART_TX_GPIO_PIN,
                             BOARD_MODBUS_UART_RX_GPIO_PORT,
                             BOARD_MODBUS_UART_RX_GPIO_PIN,
                             GPIO_AF7_USART3);

    /* USART3 DMA Init */
    /* USART3_RX Init */
    hdma_usart3_rx.Instance = DMA1_Stream1;
    hdma_usart3_rx.Init.Channel = DMA_CHANNEL_4;
    hdma_usart3_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_usart3_rx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_usart3_rx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_usart3_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart3_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma_usart3_rx.Init.Mode = DMA_CIRCULAR;
    hdma_usart3_rx.Init.Priority = DMA_PRIORITY_LOW;
    hdma_usart3_rx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_usart3_rx) != HAL_OK)
    {
      Error_Handler();
    }

    __HAL_LINKDMA(huart,hdmarx,hdma_usart3_rx);

    /* USART3 interrupt Init */
    HAL_NVIC_SetPriority(USART3_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
    /* USER CODE BEGIN USART3_MspInit 1 */

    /* USER CODE END USART3_MspInit 1 */
  }
#if BOARD_HAS_CELLULAR
  else if(huart->Instance==USART6)
  {
    /* USER CODE BEGIN USART6_MspInit 0 */

    /* USER CODE END USART6_MspInit 0 */
    /* Peripheral clock enable */
    __HAL_RCC_USART6_CLK_ENABLE();

    board_msp_uart_gpio_init(BOARD_CELLULAR_UART_TX_GPIO_PORT,
                             BOARD_CELLULAR_UART_TX_GPIO_PIN,
                             BOARD_CELLULAR_UART_RX_GPIO_PORT,
                             BOARD_CELLULAR_UART_RX_GPIO_PIN,
                             GPIO_AF8_USART6);

    /* USART6 DMA Init */
    /* USART6_RX Init */
    hdma_usart6_rx.Instance = DMA2_Stream1;
    hdma_usart6_rx.Init.Channel = DMA_CHANNEL_5;
    hdma_usart6_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_usart6_rx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_usart6_rx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_usart6_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart6_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma_usart6_rx.Init.Mode = DMA_CIRCULAR;
    hdma_usart6_rx.Init.Priority = DMA_PRIORITY_HIGH;
    hdma_usart6_rx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_usart6_rx) != HAL_OK)
    {
      Error_Handler();
    }

    __HAL_LINKDMA(huart,hdmarx,hdma_usart6_rx);

    /* USART6_TX Init */
    hdma_usart6_tx.Instance = DMA2_Stream6;
    hdma_usart6_tx.Init.Channel = DMA_CHANNEL_5;
    hdma_usart6_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_usart6_tx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_usart6_tx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_usart6_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart6_tx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma_usart6_tx.Init.Mode = DMA_NORMAL;
    hdma_usart6_tx.Init.Priority = DMA_PRIORITY_MEDIUM;
    hdma_usart6_tx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_usart6_tx) != HAL_OK)
    {
      Error_Handler();
    }

    __HAL_LINKDMA(huart,hdmatx,hdma_usart6_tx);

    /* USART6 interrupt Init */
    HAL_NVIC_SetPriority(USART6_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART6_IRQn);
    /* USER CODE BEGIN USART6_MspInit 1 */

    /* USER CODE END USART6_MspInit 1 */
  }
#endif

}

/**
  * @brief UART MSP De-Initialization
  * This function freeze the hardware resources used in this example
  * @param huart: UART handle pointer
  * @retval None
  */
void HAL_UART_MspDeInit(UART_HandleTypeDef* huart)
{
  if(huart->Instance==USART1)
  {
    /* USER CODE BEGIN USART1_MspDeInit 0 */

    /* USER CODE END USART1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USART1_CLK_DISABLE();

    board_msp_uart_gpio_deinit(BOARD_DEBUG_UART_TX_GPIO_PORT,
                               BOARD_DEBUG_UART_TX_GPIO_PIN,
                               BOARD_DEBUG_UART_RX_GPIO_PORT,
                               BOARD_DEBUG_UART_RX_GPIO_PIN);

    /* USER CODE BEGIN USART1_MspDeInit 1 */

    /* USER CODE END USART1_MspDeInit 1 */
  }
  else if(huart->Instance==USART3)
  {
    /* USER CODE BEGIN USART3_MspDeInit 0 */

    /* USER CODE END USART3_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USART3_CLK_DISABLE();

    board_msp_uart_gpio_deinit(BOARD_MODBUS_UART_TX_GPIO_PORT,
                               BOARD_MODBUS_UART_TX_GPIO_PIN,
                               BOARD_MODBUS_UART_RX_GPIO_PORT,
                               BOARD_MODBUS_UART_RX_GPIO_PIN);

    /* USART3 DMA DeInit */
    HAL_DMA_DeInit(huart->hdmarx);

    /* USART3 interrupt DeInit */
    HAL_NVIC_DisableIRQ(USART3_IRQn);
    /* USER CODE BEGIN USART3_MspDeInit 1 */

    /* USER CODE END USART3_MspDeInit 1 */
  }
#if BOARD_HAS_CELLULAR
  else if(huart->Instance==USART6)
  {
    /* USER CODE BEGIN USART6_MspDeInit 0 */

    /* USER CODE END USART6_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USART6_CLK_DISABLE();

    board_msp_uart_gpio_deinit(BOARD_CELLULAR_UART_TX_GPIO_PORT,
                               BOARD_CELLULAR_UART_TX_GPIO_PIN,
                               BOARD_CELLULAR_UART_RX_GPIO_PORT,
                               BOARD_CELLULAR_UART_RX_GPIO_PIN);

    /* USART6 DMA DeInit */
    HAL_DMA_DeInit(huart->hdmarx);
    HAL_DMA_DeInit(huart->hdmatx);

    /* USART6 interrupt DeInit */
    HAL_NVIC_DisableIRQ(USART6_IRQn);
    /* USER CODE BEGIN USART6_MspDeInit 1 */

    /* USER CODE END USART6_MspDeInit 1 */
  }
#endif

}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
