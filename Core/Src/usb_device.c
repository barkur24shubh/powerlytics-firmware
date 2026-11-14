#include "main.h"
#include "usb_device.h"
#include "usbd_desc.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"

USBD_HandleTypeDef hUsbDeviceFS;

void MX_USB_DEVICE_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    /*
     * Force an unambiguous USB disconnect before enabling the device stack.
     * A warm MCU reset can otherwise leave macOS holding the previous CDC
     * session because the FS pull-up disappears for too short a time.
     */
    __HAL_RCC_USB_OTG_FS_FORCE_RESET();
    __HAL_RCC_USB_OTG_FS_RELEASE_RESET();
    __HAL_RCC_USB_OTG_FS_CLK_DISABLE();

    __HAL_RCC_GPIOA_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_11 | GPIO_PIN_12;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &gpio);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_11 | GPIO_PIN_12, GPIO_PIN_RESET);
    HAL_Delay(1000U);
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_11 | GPIO_PIN_12);

    if (USBD_Init(&hUsbDeviceFS, &VCP_Desc, 0U) != USBD_OK)
        Error_Handler();
    if (USBD_RegisterClass(&hUsbDeviceFS, USBD_CDC_CLASS) != USBD_OK)
        Error_Handler();
    if (USBD_CDC_RegisterInterface(&hUsbDeviceFS, &USBD_CDC_fops) != USBD_OK)
        Error_Handler();

    HAL_PCD_DevDisconnect((PCD_HandleTypeDef *)hUsbDeviceFS.pData);
    HAL_Delay(300U);

    if (USBD_Start(&hUsbDeviceFS) != USBD_OK)
        Error_Handler();
}
