#include "usb_device.h"
#include "usbd_cdc_if.h"

static uint8_t UserRxBufferFS[64];
static uint8_t UserTxBufferFS[1024];

static int8_t CDC_Init_FS(void)
{
    if (USBD_CDC_SetTxBuffer(&hUsbDeviceFS, UserTxBufferFS, 0U) != USBD_OK)
        return (int8_t)USBD_FAIL;
    if (USBD_CDC_SetRxBuffer(&hUsbDeviceFS, UserRxBufferFS) != USBD_OK)
        return (int8_t)USBD_FAIL;
    return (int8_t)USBD_OK;
}
static int8_t CDC_DeInit_FS(void) { return (int8_t)USBD_OK; }
static int8_t CDC_Control_FS(uint8_t cmd, uint8_t *buffer, uint16_t length)
{
    (void)cmd; (void)buffer; (void)length;
    return (int8_t)USBD_OK;
}
static int8_t CDC_Receive_FS(uint8_t *buffer, uint32_t *length)
{
    (void)length;
    USBD_CDC_SetRxBuffer(&hUsbDeviceFS, buffer);
    USBD_CDC_ReceivePacket(&hUsbDeviceFS);
    return (int8_t)USBD_OK;
}
static int8_t CDC_TransmitCplt_FS(uint8_t *buffer, uint32_t *length, uint8_t epnum)
{
    (void)buffer; (void)length; (void)epnum;
    return (int8_t)USBD_OK;
}

USBD_CDC_ItfTypeDef USBD_CDC_fops = {
    CDC_Init_FS, CDC_DeInit_FS, CDC_Control_FS, CDC_Receive_FS, CDC_TransmitCplt_FS
};

uint8_t CDC_Transmit_FS(uint8_t *buffer, uint16_t length)
{
    if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED) return (uint8_t)USBD_FAIL;
    USBD_CDC_HandleTypeDef *cdc =
        (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
    if ((cdc == NULL) || (cdc->TxState != 0U)) return (uint8_t)USBD_BUSY;
    if (USBD_CDC_SetTxBuffer(&hUsbDeviceFS, UserTxBufferFS, 0U) != USBD_OK)
        return (uint8_t)USBD_FAIL;
    if (length > sizeof(UserTxBufferFS)) return (uint8_t)USBD_FAIL;
    memcpy(UserTxBufferFS, buffer, length);
    if (USBD_CDC_SetTxBuffer(&hUsbDeviceFS, UserTxBufferFS, length) != USBD_OK)
        return (uint8_t)USBD_FAIL;
    return USBD_CDC_TransmitPacket(&hUsbDeviceFS);
}
