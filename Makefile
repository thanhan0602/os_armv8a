CROSS_COMPILE ?= aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy

SRC_DIR := src
BUILD_DIR := build
TARGET_ELF := $(BUILD_DIR)/kernel8.elf
TARGET_IMG := $(BUILD_DIR)/kernel8.img
LINKER_SCRIPT := $(SRC_DIR)/linker.ld
QEMU_BIN ?= /home/a/qemu/build/qemu-system-aarch64

CFLAGS := -Wall -Wextra -O2 -g -ffreestanding -fno-builtin -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Isrc/include -MMD -MP
ASFLAGS := -g -ffreestanding -MMD -MP
LDFLAGS := -nostdlib -nostartfiles -nodefaultlibs -static -no-pie -Wl,--build-id=none

DEBUG_PRE_MMU ?= 1
DEBUG_MMU_BOOT ?= 1
DEBUG_POST_MMU ?= 1
KERNEL_VIRTUAL ?= 1

CFLAGS += -DKERNEL_DEBUG_ENABLE_PRE_MMU=$(DEBUG_PRE_MMU)
CFLAGS += -DKERNEL_DEBUG_ENABLE_MMU_BOOT=$(DEBUG_MMU_BOOT)
CFLAGS += -DKERNEL_DEBUG_ENABLE_POST_MMU=$(DEBUG_POST_MMU)

ifeq ($(KERNEL_VIRTUAL),1)
CFLAGS += -DCONFIG_KERNEL_VIRTUAL=1
ASFLAGS += -DCONFIG_KERNEL_VIRTUAL=1
endif

C_SOURCES := $(shell find $(SRC_DIR) -type f -name '*.c')
S_SOURCES := $(shell find $(SRC_DIR) -type f -name '*.S')

C_OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%_c.o,$(C_SOURCES))
S_OBJECTS := $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%_s.o,$(S_SOURCES))
OBJECTS := $(C_OBJECTS) $(S_OBJECTS)
DEPS := $(OBJECTS:.o=.d)

all: $(TARGET_IMG)

$(TARGET_IMG): $(TARGET_ELF)
	$(OBJCOPY) -O binary $< $@

$(TARGET_ELF): $(OBJECTS) $(LINKER_SCRIPT)
	$(CC) $(LDFLAGS) -T $(LINKER_SCRIPT) -o $@ $(OBJECTS)

$(BUILD_DIR)/%_c.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%_s.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

run: $(TARGET_IMG)
	QEMU_BIN=$(QEMU_BIN) KERNEL_IMG=$(TARGET_IMG) \
	bash scripts/run_qemu.sh

debug: $(TARGET_IMG)
	QEMU_BIN=$(QEMU_BIN) KERNEL_IMG=$(TARGET_IMG) \
	bash scripts/run_qemu_debug.sh

-include $(DEPS)

.PHONY: all clean run debug