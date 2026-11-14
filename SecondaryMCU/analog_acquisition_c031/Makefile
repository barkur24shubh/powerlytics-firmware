PROJECT := analog_acquisition_c031
BUILD := build
CC := /Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.macos64_1.0.0.202411102158/tools/bin/arm-none-eabi-gcc
OBJCOPY := $(dir $(CC))arm-none-eabi-objcopy
SIZE := $(dir $(CC))arm-none-eabi-size

CUBE := Vendor/STM32CubeC0
HAL := $(CUBE)/Drivers/STM32C0xx_HAL_Driver
CMSIS := $(CUBE)/Drivers/CMSIS
DEVICE := $(CMSIS)/Device/ST/STM32C0xx

C_SOURCES := \
 Src/main.c Src/stm32c0xx_it.c Src/system_stm32c0xx.c \
 $(HAL)/Src/stm32c0xx_hal.c $(HAL)/Src/stm32c0xx_hal_adc.c \
 $(HAL)/Src/stm32c0xx_hal_adc_ex.c $(HAL)/Src/stm32c0xx_hal_cortex.c \
 $(HAL)/Src/stm32c0xx_hal_dma.c $(HAL)/Src/stm32c0xx_hal_dma_ex.c \
 $(HAL)/Src/stm32c0xx_hal_exti.c $(HAL)/Src/stm32c0xx_hal_flash.c \
 $(HAL)/Src/stm32c0xx_hal_flash_ex.c $(HAL)/Src/stm32c0xx_hal_gpio.c \
 $(HAL)/Src/stm32c0xx_hal_rcc.c $(HAL)/Src/stm32c0xx_hal_rcc_ex.c \
 $(HAL)/Src/stm32c0xx_hal_uart.c $(HAL)/Src/stm32c0xx_hal_uart_ex.c
ASM_SOURCES := Startup/startup_stm32c031xx.s
OBJECTS := $(addprefix $(BUILD)/,$(C_SOURCES:.c=.o)) $(addprefix $(BUILD)/,$(ASM_SOURCES:.s=.o))

INCLUDES := -IInc -I$(HAL)/Inc -I$(HAL)/Inc/Legacy -I$(CMSIS)/Include -I$(DEVICE)/Include
CFLAGS := -mcpu=cortex-m0plus -mthumb -std=gnu11 -DSTM32C031xx -DUSE_HAL_DRIVER \
 -Os -ffunction-sections -fdata-sections -Wall -Wextra $(INCLUDES)
LDFLAGS := -mcpu=cortex-m0plus -mthumb -TSTM32C031C6TX_FLASH.ld \
 --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(PROJECT).map

.PHONY: all clean
all: $(BUILD)/$(PROJECT).elf $(BUILD)/$(PROJECT).hex $(BUILD)/$(PROJECT).bin

$(BUILD)/$(PROJECT).elf: $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -x assembler-with-cpp -c $< -o $@

$(BUILD)/$(PROJECT).hex: $(BUILD)/$(PROJECT).elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/$(PROJECT).bin: $(BUILD)/$(PROJECT).elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(BUILD)

-include $(OBJECTS:.o=.d)
