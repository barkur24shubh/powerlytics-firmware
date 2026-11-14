/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app_gateway_platform.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/*
 * Keep the RTOS allocator out of normal SRAM. Boot runs on MSP before the
 * scheduler starts, and large filesystem/debug call frames otherwise collide
 * with a heap placed at the top of the 128 KB DMA-capable SRAM region.
 * FreeRTOS objects and task stacks are CPU-only and are safe in CCM RAM.
 */
uint8_t ucHeap[configTOTAL_HEAP_SIZE]
  __attribute__((section(".ccmram"), aligned(8)));

/* USER CODE END Variables */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
void vApplicationMallocFailedHook(void)
{
  app_gateway_fault_capture_and_reset(NULL,
                                      APP_GATEWAY_FAULT_MALLOC_FAILED,
                                      0U);
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
  (void)pcTaskName;
  app_gateway_fault_capture_and_reset(NULL,
                                      APP_GATEWAY_FAULT_STACK_OVERFLOW,
                                      (uint32_t)(uintptr_t)xTask);
}

/* USER CODE END Application */

