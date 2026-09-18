PREFIX   = arm-none-eabi-
CC       = $(PREFIX)gcc
OBJCOPY  = $(PREFIX)objcopy
CFLAGS   = -mcpu=cortex-m3 -mthumb -g -Wall
CFLAGS  += -Iinclude
CFLAGS  += -ICMSIS 
CFLAGS  += -IFreeRTOS/include -IFreeRTOS/Source/include
CFLAGS  += -IFreeRTOS/portable/GCC/ARM_CM3
CFLAGS  += -DSTM32F103xB
CFLAGS  += -ffunction-sections -fdata-sections
CFLAGS  += -MMD -MP

BUILD_DIR = build
TARGET    = app

LDFLAGS  = -T src/STM32F103XB_FLASH.ld -nostartfiles 
LDFLAGS += -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/$(TARGET).map
LDFLAGS += --specs=nano.specs --specs=nosys.specs

SRCS =  src/main.c \
        CMSIS/startup_stm32f103xb.s \
        CMSIS/system_stm32f1xx.c \
        CMSIS/syscalls.c \
        FreeRTOS/list.c \
        FreeRTOS/queue.c \
        FreeRTOS/tasks.c \
        FreeRTOS/timers.c \
        FreeRTOS/portable/MemMang/heap_4.c \
        FreeRTOS/portable/GCC/ARM_CM3/port.c

OBJS = $(addprefix $(BUILD_DIR)/,$(SRCS:.c=.o))
OBJS := $(OBJS:.s=.o)

DEPS = $(OBJS:.o=.d)

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/$(TARGET).hex

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: %.s | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	
$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf
	$(OBJCOPY) -O binary $< $@
	
$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf
	$(OBJCOPY) -O ihex $< $@

flash: $(BUILD_DIR)/$(TARGET).elf
	openocd -f interface/stlink.cfg -f target/stm32f1x.cfg -c "program $< verify reset exit"

flash_clone: $(BUILD_DIR)/$(TARGET).elf
	openocd -f interface/stlink.cfg -c "set CPUTAPID 0x2ba01477" -f target/stm32f1x.cfg -c "program $< verify reset exit"

launch: flash
	@echo
	@echo "ejecutar \"make attach\""
	@echo
	openocd -f interface/stlink.cfg -f target/stm32f1x.cfg

launch_clone: flash_clone
	@echo
	@echo "ejecutar \"make attach\""
	@echo
	openocd -f interface/stlink.cfg -c "set CPUTAPID 0x2ba01477" -f target/stm32f1x.cfg

attach:
	gdb $(BUILD_DIR)/$(TARGET).elf --ex "target extended-remote :3333"

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)

.PHONY: all flash clean
