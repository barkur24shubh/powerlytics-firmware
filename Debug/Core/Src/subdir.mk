################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Core/Src/app_board_io.c \
../Core/Src/app_debug_led.c \
../Core/Src/app_flash_log.c \
../Core/Src/app_gateway_platform.c \
../Core/Src/app_gateway_runtime.c \
../Core/Src/app_health.c \
../Core/Src/app_log.c \
../Core/Src/app_modbus_payload.c \
../Core/Src/app_powerlytics_engine.c \
../Core/Src/app_rule_eval.c \
../Core/Src/app_rule_action.c \
../Core/Src/app_sd_card.c \
../Core/Src/app_watchdog.c \
../Core/Src/bsp_driver_sd.c \
../Core/Src/cJSON.c \
../Core/Src/comm_manager.c \
../Core/Src/connectivity_cellular.c \
../Core/Src/connectivity_ethernet.c \
../Core/Src/connectivity_manager.c \
../Core/Src/connectivity_wifi.c \
../Core/Src/config_apply.c \
../Core/Src/debug_uart.c \
../Core/Src/diskio.c \
../Core/Src/fatfs.c \
../Core/Src/fatfs_platform.c \
../Core/Src/ff.c \
../Core/Src/ff_gen_drv.c \
../Core/Src/ffunicode.c \
../Core/Src/freertos.c \
../Core/Src/main.c \
../Core/Src/modbus_config_flash.c \
../Core/Src/modbus_config_json.c \
../Core/Src/modbus_crc.c \
../Core/Src/modbus_rtu.c \
../Core/Src/modbus_rtu_master.c \
../Core/Src/modem_handler.c \
../Core/Src/modem_urc.c \
../Core/Src/mqtt_config.c \
../Core/Src/mqtt_handler.c \
../Core/Src/pub_queue.c \
../Core/Src/rs4585.c \
../Core/Src/rtc_manager.c \
../Core/Src/sd_diskio.c \
../Core/Src/secondary_adc_link.c \
../Core/Src/sha256.c \
../Core/Src/stm32f4xx_hal_msp.c \
../Core/Src/stm32f4xx_it.c \
../Core/Src/syscalls.c \
../Core/Src/sysmem.c \
../Core/Src/system_stm32f4xx.c \
../Core/Src/uart2_rx_engine.c \
../Core/Src/w25q16.c 

OBJS += \
./Core/Src/app_board_io.o \
./Core/Src/app_debug_led.o \
./Core/Src/app_flash_log.o \
./Core/Src/app_gateway_platform.o \
./Core/Src/app_gateway_runtime.o \
./Core/Src/app_health.o \
./Core/Src/app_log.o \
./Core/Src/app_modbus_payload.o \
./Core/Src/app_powerlytics_engine.o \
./Core/Src/app_rule_eval.o \
./Core/Src/app_rule_action.o \
./Core/Src/app_sd_card.o \
./Core/Src/app_watchdog.o \
./Core/Src/bsp_driver_sd.o \
./Core/Src/cJSON.o \
./Core/Src/comm_manager.o \
./Core/Src/connectivity_cellular.o \
./Core/Src/connectivity_ethernet.o \
./Core/Src/connectivity_manager.o \
./Core/Src/connectivity_wifi.o \
./Core/Src/config_apply.o \
./Core/Src/debug_uart.o \
./Core/Src/diskio.o \
./Core/Src/fatfs.o \
./Core/Src/fatfs_platform.o \
./Core/Src/ff.o \
./Core/Src/ff_gen_drv.o \
./Core/Src/ffunicode.o \
./Core/Src/freertos.o \
./Core/Src/main.o \
./Core/Src/modbus_config_flash.o \
./Core/Src/modbus_config_json.o \
./Core/Src/modbus_crc.o \
./Core/Src/modbus_rtu.o \
./Core/Src/modbus_rtu_master.o \
./Core/Src/modem_handler.o \
./Core/Src/modem_urc.o \
./Core/Src/mqtt_config.o \
./Core/Src/mqtt_handler.o \
./Core/Src/pub_queue.o \
./Core/Src/rs4585.o \
./Core/Src/rtc_manager.o \
./Core/Src/sd_diskio.o \
./Core/Src/secondary_adc_link.o \
./Core/Src/sha256.o \
./Core/Src/stm32f4xx_hal_msp.o \
./Core/Src/stm32f4xx_it.o \
./Core/Src/syscalls.o \
./Core/Src/sysmem.o \
./Core/Src/system_stm32f4xx.o \
./Core/Src/uart2_rx_engine.o \
./Core/Src/w25q16.o 

C_DEPS += \
./Core/Src/app_board_io.d \
./Core/Src/app_debug_led.d \
./Core/Src/app_flash_log.d \
./Core/Src/app_gateway_platform.d \
./Core/Src/app_gateway_runtime.d \
./Core/Src/app_health.d \
./Core/Src/app_log.d \
./Core/Src/app_modbus_payload.d \
./Core/Src/app_powerlytics_engine.d \
./Core/Src/app_rule_eval.d \
./Core/Src/app_rule_action.d \
./Core/Src/app_sd_card.d \
./Core/Src/app_watchdog.d \
./Core/Src/bsp_driver_sd.d \
./Core/Src/cJSON.d \
./Core/Src/comm_manager.d \
./Core/Src/connectivity_cellular.d \
./Core/Src/connectivity_ethernet.d \
./Core/Src/connectivity_manager.d \
./Core/Src/connectivity_wifi.d \
./Core/Src/config_apply.d \
./Core/Src/debug_uart.d \
./Core/Src/diskio.d \
./Core/Src/fatfs.d \
./Core/Src/fatfs_platform.d \
./Core/Src/ff.d \
./Core/Src/ff_gen_drv.d \
./Core/Src/ffunicode.d \
./Core/Src/freertos.d \
./Core/Src/main.d \
./Core/Src/modbus_config_flash.d \
./Core/Src/modbus_config_json.d \
./Core/Src/modbus_crc.d \
./Core/Src/modbus_rtu.d \
./Core/Src/modbus_rtu_master.d \
./Core/Src/modem_handler.d \
./Core/Src/modem_urc.d \
./Core/Src/mqtt_config.d \
./Core/Src/mqtt_handler.d \
./Core/Src/pub_queue.d \
./Core/Src/rs4585.d \
./Core/Src/rtc_manager.d \
./Core/Src/sd_diskio.d \
./Core/Src/secondary_adc_link.d \
./Core/Src/sha256.d \
./Core/Src/stm32f4xx_hal_msp.d \
./Core/Src/stm32f4xx_it.d \
./Core/Src/syscalls.d \
./Core/Src/sysmem.d \
./Core/Src/system_stm32f4xx.d \
./Core/Src/uart2_rx_engine.d \
./Core/Src/w25q16.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Src/%.o Core/Src/%.su Core/Src/%.cyclo: ../Core/Src/%.c Core/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F407xx -c -I../Core/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -I../Middlewares/Third_Party/FreeRTOS/Source/include -I../Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2 -I../Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F -I../Middlewares/ST/STM32_USB_Device_Library/Core/Inc -I../Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Core-2f-Src

clean-Core-2f-Src:
	-$(RM) ./Core/Src/app_board_io.cyclo ./Core/Src/app_board_io.d ./Core/Src/app_board_io.o ./Core/Src/app_board_io.su ./Core/Src/app_debug_led.cyclo ./Core/Src/app_debug_led.d ./Core/Src/app_debug_led.o ./Core/Src/app_debug_led.su ./Core/Src/app_flash_log.cyclo ./Core/Src/app_flash_log.d ./Core/Src/app_flash_log.o ./Core/Src/app_flash_log.su ./Core/Src/app_gateway_platform.cyclo ./Core/Src/app_gateway_platform.d ./Core/Src/app_gateway_platform.o ./Core/Src/app_gateway_platform.su ./Core/Src/app_gateway_runtime.cyclo ./Core/Src/app_gateway_runtime.d ./Core/Src/app_gateway_runtime.o ./Core/Src/app_gateway_runtime.su ./Core/Src/app_health.cyclo ./Core/Src/app_health.d ./Core/Src/app_health.o ./Core/Src/app_health.su ./Core/Src/app_log.cyclo ./Core/Src/app_log.d ./Core/Src/app_log.o ./Core/Src/app_log.su ./Core/Src/app_modbus_payload.cyclo ./Core/Src/app_modbus_payload.d ./Core/Src/app_modbus_payload.o ./Core/Src/app_modbus_payload.su ./Core/Src/app_powerlytics_engine.cyclo ./Core/Src/app_powerlytics_engine.d ./Core/Src/app_powerlytics_engine.o ./Core/Src/app_powerlytics_engine.su ./Core/Src/app_rule_eval.cyclo ./Core/Src/app_rule_eval.d ./Core/Src/app_rule_eval.o ./Core/Src/app_rule_eval.su ./Core/Src/app_rule_action.cyclo ./Core/Src/app_rule_action.d ./Core/Src/app_rule_action.o ./Core/Src/app_rule_action.su ./Core/Src/app_sd_card.cyclo ./Core/Src/app_sd_card.d ./Core/Src/app_sd_card.o ./Core/Src/app_sd_card.su ./Core/Src/app_watchdog.cyclo ./Core/Src/app_watchdog.d ./Core/Src/app_watchdog.o ./Core/Src/app_watchdog.su ./Core/Src/bsp_driver_sd.cyclo ./Core/Src/bsp_driver_sd.d ./Core/Src/bsp_driver_sd.o ./Core/Src/bsp_driver_sd.su ./Core/Src/cJSON.cyclo ./Core/Src/cJSON.d ./Core/Src/cJSON.o ./Core/Src/cJSON.su ./Core/Src/comm_manager.cyclo ./Core/Src/comm_manager.d ./Core/Src/comm_manager.o ./Core/Src/comm_manager.su ./Core/Src/connectivity_cellular.cyclo ./Core/Src/connectivity_cellular.d ./Core/Src/connectivity_cellular.o ./Core/Src/connectivity_cellular.su ./Core/Src/connectivity_ethernet.cyclo ./Core/Src/connectivity_ethernet.d ./Core/Src/connectivity_ethernet.o ./Core/Src/connectivity_ethernet.su ./Core/Src/connectivity_manager.cyclo ./Core/Src/connectivity_manager.d ./Core/Src/connectivity_manager.o ./Core/Src/connectivity_manager.su ./Core/Src/connectivity_wifi.cyclo ./Core/Src/connectivity_wifi.d ./Core/Src/connectivity_wifi.o ./Core/Src/connectivity_wifi.su ./Core/Src/config_apply.cyclo ./Core/Src/config_apply.d ./Core/Src/config_apply.o ./Core/Src/config_apply.su ./Core/Src/debug_uart.cyclo ./Core/Src/debug_uart.d ./Core/Src/debug_uart.o ./Core/Src/debug_uart.su ./Core/Src/diskio.cyclo ./Core/Src/diskio.d ./Core/Src/diskio.o ./Core/Src/diskio.su ./Core/Src/fatfs.cyclo ./Core/Src/fatfs.d ./Core/Src/fatfs.o ./Core/Src/fatfs.su ./Core/Src/fatfs_platform.cyclo ./Core/Src/fatfs_platform.d ./Core/Src/fatfs_platform.o ./Core/Src/fatfs_platform.su ./Core/Src/ff.cyclo ./Core/Src/ff.d ./Core/Src/ff.o ./Core/Src/ff.su ./Core/Src/ff_gen_drv.cyclo ./Core/Src/ff_gen_drv.d ./Core/Src/ff_gen_drv.o ./Core/Src/ff_gen_drv.su ./Core/Src/ffunicode.cyclo ./Core/Src/ffunicode.d ./Core/Src/ffunicode.o ./Core/Src/ffunicode.su ./Core/Src/freertos.cyclo ./Core/Src/freertos.d ./Core/Src/freertos.o ./Core/Src/freertos.su ./Core/Src/main.cyclo ./Core/Src/main.d ./Core/Src/main.o ./Core/Src/main.su ./Core/Src/modbus_config_flash.cyclo ./Core/Src/modbus_config_flash.d ./Core/Src/modbus_config_flash.o ./Core/Src/modbus_config_flash.su ./Core/Src/modbus_config_json.cyclo ./Core/Src/modbus_config_json.d ./Core/Src/modbus_config_json.o ./Core/Src/modbus_config_json.su ./Core/Src/modbus_crc.cyclo ./Core/Src/modbus_crc.d ./Core/Src/modbus_crc.o ./Core/Src/modbus_crc.su ./Core/Src/modbus_rtu.cyclo ./Core/Src/modbus_rtu.d ./Core/Src/modbus_rtu.o ./Core/Src/modbus_rtu.su ./Core/Src/modbus_rtu_master.cyclo ./Core/Src/modbus_rtu_master.d ./Core/Src/modbus_rtu_master.o ./Core/Src/modbus_rtu_master.su ./Core/Src/modem_handler.cyclo ./Core/Src/modem_handler.d ./Core/Src/modem_handler.o ./Core/Src/modem_handler.su ./Core/Src/modem_urc.cyclo ./Core/Src/modem_urc.d ./Core/Src/modem_urc.o ./Core/Src/modem_urc.su ./Core/Src/mqtt_config.cyclo ./Core/Src/mqtt_config.d ./Core/Src/mqtt_config.o ./Core/Src/mqtt_config.su ./Core/Src/mqtt_handler.cyclo ./Core/Src/mqtt_handler.d ./Core/Src/mqtt_handler.o ./Core/Src/mqtt_handler.su ./Core/Src/pub_queue.cyclo ./Core/Src/pub_queue.d ./Core/Src/pub_queue.o ./Core/Src/pub_queue.su ./Core/Src/rs4585.cyclo ./Core/Src/rs4585.d ./Core/Src/rs4585.o ./Core/Src/rs4585.su ./Core/Src/rtc_manager.cyclo ./Core/Src/rtc_manager.d ./Core/Src/rtc_manager.o ./Core/Src/rtc_manager.su ./Core/Src/sd_diskio.cyclo ./Core/Src/sd_diskio.d ./Core/Src/sd_diskio.o ./Core/Src/sd_diskio.su ./Core/Src/secondary_adc_link.cyclo ./Core/Src/secondary_adc_link.d ./Core/Src/secondary_adc_link.o ./Core/Src/secondary_adc_link.su ./Core/Src/sha256.cyclo ./Core/Src/sha256.d ./Core/Src/sha256.o ./Core/Src/sha256.su ./Core/Src/stm32f4xx_hal_msp.cyclo ./Core/Src/stm32f4xx_hal_msp.d ./Core/Src/stm32f4xx_hal_msp.o ./Core/Src/stm32f4xx_hal_msp.su ./Core/Src/stm32f4xx_it.cyclo ./Core/Src/stm32f4xx_it.d ./Core/Src/stm32f4xx_it.o ./Core/Src/stm32f4xx_it.su ./Core/Src/syscalls.cyclo ./Core/Src/syscalls.d ./Core/Src/syscalls.o ./Core/Src/syscalls.su ./Core/Src/sysmem.cyclo ./Core/Src/sysmem.d ./Core/Src/sysmem.o ./Core/Src/sysmem.su ./Core/Src/system_stm32f4xx.cyclo ./Core/Src/system_stm32f4xx.d ./Core/Src/system_stm32f4xx.o ./Core/Src/system_stm32f4xx.su ./Core/Src/uart2_rx_engine.cyclo ./Core/Src/uart2_rx_engine.d ./Core/Src/uart2_rx_engine.o ./Core/Src/uart2_rx_engine.su ./Core/Src/w25q16.cyclo ./Core/Src/w25q16.d ./Core/Src/w25q16.o ./Core/Src/w25q16.su

.PHONY: clean-Core-2f-Src
