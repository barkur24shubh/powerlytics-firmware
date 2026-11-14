################################################################################
# USB CDC debug transport sources.
################################################################################

USB_CDC_INC := \
-I../Core/Inc \
-I../Drivers/STM32F4xx_HAL_Driver/Inc \
-I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy \
-I../Drivers/CMSIS/Device/ST/STM32F4xx/Include \
-I../Drivers/CMSIS/Include \
-I../Middlewares/ST/STM32_USB_Device_Library/Core/Inc \
-I../Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc

USB_CDC_CFLAGS := -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -c \
-DSTM32F407xx -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage \
-fcyclomatic-complexity -MMD -MP --specs=nano.specs -mfpu=fpv4-sp-d16 \
-mfloat-abi=hard -mthumb $(USB_CDC_INC)

USB_CDC_CORE_SRCS := usb_device usbd_cdc_if usbd_conf usbd_desc
USB_CDC_HAL_SRCS := stm32f4xx_hal_pcd stm32f4xx_hal_pcd_ex stm32f4xx_ll_usb
USB_CDC_LIB_CORE_SRCS := usbd_core usbd_ctlreq usbd_ioreq

OBJS += $(addprefix ./Core/Src/,$(addsuffix .o,$(USB_CDC_CORE_SRCS)))
OBJS += $(addprefix ./Drivers/STM32F4xx_HAL_Driver/Src/,$(addsuffix .o,$(USB_CDC_HAL_SRCS)))
OBJS += $(addprefix ./Middlewares/ST/STM32_USB_Device_Library/Core/Src/,$(addsuffix .o,$(USB_CDC_LIB_CORE_SRCS)))
OBJS += ./Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.o

Core/Src/usb_device.o Core/Src/usbd_cdc_if.o Core/Src/usbd_conf.o Core/Src/usbd_desc.o: Core/Src/%.o: ../Core/Src/%.c
	arm-none-eabi-gcc "$<" $(USB_CDC_CFLAGS) -MF"$(@:%.o=%.d)" -MT"$@" -o "$@"

Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pcd.o \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pcd_ex.o \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_usb.o: Drivers/STM32F4xx_HAL_Driver/Src/%.o: ../Drivers/STM32F4xx_HAL_Driver/Src/%.c
	arm-none-eabi-gcc "$<" $(USB_CDC_CFLAGS) -MF"$(@:%.o=%.d)" -MT"$@" -o "$@"

Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_core.o \
Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ctlreq.o \
Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ioreq.o: Middlewares/ST/STM32_USB_Device_Library/Core/Src/%.o: ../Middlewares/ST/STM32_USB_Device_Library/Core/Src/%.c
	@mkdir -p "$(@D)"
	arm-none-eabi-gcc "$<" $(USB_CDC_CFLAGS) -MF"$(@:%.o=%.d)" -MT"$@" -o "$@"

Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.o: ../Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.c
	@mkdir -p "$(@D)"
	arm-none-eabi-gcc "$<" $(USB_CDC_CFLAGS) -MF"$(@:%.o=%.d)" -MT"$@" -o "$@"
