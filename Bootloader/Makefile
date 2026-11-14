TARGET = powerlytics_bootloader

BUILD_DIR = Debug

CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size

CFLAGS = -mcpu=cortex-m4 -std=gnu11 -Os -ffunction-sections -fdata-sections
CFLAGS += -Wall -Wextra -Werror
CFLAGS += -DSTM32F407xx
CFLAGS += -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include
CFLAGS += -I../Drivers/CMSIS/Include
CFLAGS += -I../Core/Inc
CFLAGS += -I../ThirdParty/monocypher
CFLAGS += -IInc
CFLAGS += -mthumb
CFLAGS += -DOTA_SIGNING_PUBLIC_KEY_HEX=\"$(OTA_SIGNING_PUBLIC_KEY_HEX)\"
CFLAGS += -DOTA_DEVICE_MODEL=\"$(POWERLYTICS_BOARD_SLUG)\"

LDFLAGS = -mcpu=cortex-m4 -mthumb
LDFLAGS += -TSTM32F407VETX_BOOTLOADER.ld
LDFLAGS += -Wl,-Map=$(BUILD_DIR)/$(TARGET).map
LDFLAGS += -Wl,--gc-sections
LDFLAGS += --specs=nano.specs --specs=nosys.specs

OBJS = \
	$(BUILD_DIR)/Startup/startup_stm32f407vetx_boot.o \
	$(BUILD_DIR)/Src/bootloader.o \
	$(BUILD_DIR)/Src/ota_signature.o \
	$(BUILD_DIR)/Core/Src/sha256.o \
	$(BUILD_DIR)/ThirdParty/monocypher/monocypher.o \
	$(BUILD_DIR)/ThirdParty/monocypher/monocypher-ed25519.o

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/$(TARGET).hex
	$(SIZE) $(BUILD_DIR)/$(TARGET).elf

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)/Startup $(BUILD_DIR)/Src $(BUILD_DIR)/Core/Src

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.s | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/Core/Src/%.o: ../Core/Src/%.c | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ThirdParty/monocypher/%.o: ../ThirdParty/monocypher/%.c | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/$(TARGET).hex: $(BUILD_DIR)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
