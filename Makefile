CROSS_COMPILE ?= aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
AR := $(CROSS_COMPILE)ar
OBJCOPY := $(CROSS_COMPILE)objcopy

SRC_DIR := src
USER_DIR := user
BUILD_DIR := build
TARGET_ELF := $(BUILD_DIR)/kernel8.elf
TARGET_IMG := $(BUILD_DIR)/kernel8.img
LINKER_SCRIPT := $(SRC_DIR)/linker.ld
USER_LINKER_SCRIPT := $(USER_DIR)/linker.ld
QEMU_BIN ?= /home/a/qemu/build/qemu-system-aarch64
USER_SHARED_LDFLAGS := -nostdlib -nostartfiles -nodefaultlibs -shared -Wl,--build-id=none -Wl,-z,max-page-size=0x1000

CFLAGS := -Wall -Wextra -O2 -g -ffreestanding -fno-builtin -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-jump-tables -Isrc/include -MMD -MP
ASFLAGS := -g -ffreestanding -MMD -MP
LDFLAGS := -nostdlib -nostartfiles -nodefaultlibs -static -no-pie -Wl,--build-id=none
USER_CFLAGS := -Wall -Wextra -O2 -g -ffreestanding -fno-builtin -fpie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-jump-tables -I$(USER_DIR)/include -MMD -MP
USER_ASFLAGS := -g -ffreestanding -I$(USER_DIR)/include -MMD -MP
USER_LDFLAGS := -nostdlib -nostartfiles -nodefaultlibs -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -T $(USER_LINKER_SCRIPT)
USER_DYNAMIC_APP_LDFLAGS := -nostdlib -nostartfiles -nodefaultlibs -pie -Wl,--build-id=none -Wl,--dynamic-linker,/lib/ld.so -Wl,-z,max-page-size=0x1000 -T $(USER_LINKER_SCRIPT)

USER_LIBC_SOURCES := $(shell find $(USER_DIR)/lib/libc -type f -name '*.c')
USER_LIBC_OBJECTS := $(patsubst $(USER_DIR)/lib/libc/%.c,$(BUILD_DIR)/user/lib/libc/%_c.o,$(USER_LIBC_SOURCES))
USER_LIBC_A := $(BUILD_DIR)/user/lib/libc.a
USER_LIBC_SO := $(BUILD_DIR)/user/lib/libc.so
USER_LINKER_SO := $(BUILD_DIR)/user/lib/ld.so

USER_BUILTIN_APPS := hello fault ipc_recv ipc_send test_cow test_str cpu_stress simple test_exec
USER_EXTERNAL_APPS := ticker shared_client
USER_BUILTIN_LIBS := c
USER_LINKER_OBJECTS := $(BUILD_DIR)/user/linker/ld_start_s.o $(BUILD_DIR)/user/linker/ld_main_c.o
USER_COMMON_OBJECT := $(BUILD_DIR)/user/common/start_s.o
USER_BUILTIN_ELFS := $(addprefix $(BUILD_DIR)/user/builtin/,$(addsuffix .elf,$(USER_BUILTIN_APPS)))
USER_EXTERNAL_ELFS := $(addprefix $(BUILD_DIR)/user/external/,$(addsuffix .elf,$(USER_EXTERNAL_APPS)))
USER_SHARED_LIBRARIES := $(addprefix $(BUILD_DIR)/user/lib/lib,$(addsuffix .so,$(USER_BUILTIN_LIBS))) $(USER_LINKER_SO)
USER_BUILTIN_BLOB_OBJECTS := $(addprefix $(BUILD_DIR)/user/builtin/,$(addsuffix _elf_blob.o,$(USER_BUILTIN_APPS)))
USER_BUILTIN_LIB_BLOB_OBJECTS := $(addprefix $(BUILD_DIR)/user/lib/lib,$(addsuffix _so_blob.o,$(USER_BUILTIN_LIBS))) \
                                 $(BUILD_DIR)/user/lib/ld_so_blob.o
USER_EXTERNAL_BLOB_OBJECTS := $(addprefix $(BUILD_DIR)/user/external/,$(addsuffix _elf_blob.o,$(USER_EXTERNAL_APPS)))

BUILTIN_CONFIG_C := $(BUILD_DIR)/kernel/builtin_files_config.c
BUILTIN_CONFIG_OBJ := $(BUILD_DIR)/kernel/builtin_files_config_c.o

DEBUG_PRE_MMU ?= 0
DEBUG_MMU_BOOT ?= 0
DEBUG_POST_MMU ?= 0
KERNEL_VIRTUAL ?= 1
RUN_OS_DEMOS ?= 1

CFLAGS += -DKERNEL_DEBUG_ENABLE_PRE_MMU=$(DEBUG_PRE_MMU)
CFLAGS += -DKERNEL_DEBUG_ENABLE_MMU_BOOT=$(DEBUG_MMU_BOOT)
CFLAGS += -DKERNEL_DEBUG_ENABLE_POST_MMU=$(DEBUG_POST_MMU)

ifeq ($(RUN_OS_DEMOS),1)
CFLAGS += -DCONFIG_RUN_OS_DEMOS=1
ASFLAGS += -DCONFIG_RUN_OS_DEMOS=1
endif

ifeq ($(KERNEL_VIRTUAL),1)
CFLAGS += -DCONFIG_KERNEL_VIRTUAL=1
ASFLAGS += -DCONFIG_KERNEL_VIRTUAL=1
endif

C_SOURCES := $(shell find $(SRC_DIR) -type f -name '*.c')
S_SOURCES := $(shell find $(SRC_DIR) -type f -name '*.S')

C_OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%_c.o,$(C_SOURCES))
S_OBJECTS := $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%_s.o,$(S_SOURCES))
OBJECTS := $(C_OBJECTS) $(S_OBJECTS) $(USER_BUILTIN_BLOB_OBJECTS) $(USER_BUILTIN_LIB_BLOB_OBJECTS) $(USER_EXTERNAL_BLOB_OBJECTS) $(BUILTIN_CONFIG_OBJ)
DEPS := $(C_OBJECTS:.o=.d) $(S_OBJECTS:.o=.d) $(USER_COMMON_OBJECT:.o=.d) $(addprefix $(BUILD_DIR)/user/builtin/,$(addsuffix _c.d,$(USER_BUILTIN_APPS))) $(addprefix $(BUILD_DIR)/user/external/,$(addsuffix _c.d,$(USER_EXTERNAL_APPS))) $(addprefix $(BUILD_DIR)/user/lib/,$(addsuffix _c.d,$(USER_BUILTIN_LIBS))) $(USER_LIBC_OBJECTS:.o=.d)

all: $(TARGET_IMG) $(USER_EXTERNAL_ELFS)

$(TARGET_IMG): $(TARGET_ELF)
	$(OBJCOPY) -O binary $< $@

$(TARGET_ELF): $(OBJECTS) $(LINKER_SCRIPT) $(USER_BUILTIN_ELFS) $(USER_SHARED_LIBRARIES)
	$(CC) $(LDFLAGS) -T $(LINKER_SCRIPT) -o $@ $(OBJECTS)

$(BUILD_DIR)/%_c.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%_s.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILTIN_CONFIG_C): Makefile
	@mkdir -p $(dir $@)
	@echo "Generating $@"
	@echo '#include <kernel/ramfs.h>' > $@
	@echo '#include <stddef.h>' >> $@
	@echo '' >> $@
	@$(foreach app,$(USER_BUILTIN_APPS), \
		echo 'extern unsigned char _binary_build_user_builtin_$(app)_elf_start[];' >> $@; \
		echo 'extern unsigned char _binary_build_user_builtin_$(app)_elf_end[];' >> $@; \
		echo 'REGISTER_RAMFS_BUILTIN($(app), "/bin/$(app).elf", _binary_build_user_builtin_$(app)_elf_start, _binary_build_user_builtin_$(app)_elf_end);' >> $@; \
	)
	@echo 'extern unsigned char _binary_build_user_lib_libc_so_start[];' >> $@
	@echo 'extern unsigned char _binary_build_user_lib_libc_so_end[];' >> $@
	@echo 'REGISTER_RAMFS_BUILTIN(libc, "/lib/libc.so", _binary_build_user_lib_libc_so_start, _binary_build_user_lib_libc_so_end);' >> $@
	@echo 'extern unsigned char _binary_build_user_lib_ld_so_start[];' >> $@
	@echo 'extern unsigned char _binary_build_user_lib_ld_so_end[];' >> $@
	@echo 'REGISTER_RAMFS_BUILTIN(ld, "/lib/ld.so", _binary_build_user_lib_ld_so_start, _binary_build_user_lib_ld_so_end);' >> $@
	@$(foreach lib,$(filter-out c ld,$(USER_BUILTIN_LIBS)), \
		echo 'extern unsigned char _binary_build_user_lib_lib$(lib)_so_start[];' >> $@; \
		echo 'extern unsigned char _binary_build_user_lib_lib$(lib)_so_end[];' >> $@; \
		echo 'REGISTER_RAMFS_BUILTIN(lib$(lib), "/lib/lib$(lib).so", _binary_build_user_lib_lib$(lib)_so_start, _binary_build_user_lib_lib$(lib)_so_end);' >> $@; \
	)
	@$(foreach app,$(USER_EXTERNAL_APPS), \
		echo 'extern unsigned char _binary_build_user_external_$(app)_elf_start[];' >> $@; \
		echo 'extern unsigned char _binary_build_user_external_$(app)_elf_end[];' >> $@; \
		echo 'REGISTER_RAMFS_BUILTIN($(app), "/bin/$(app).elf", _binary_build_user_external_$(app)_elf_start, _binary_build_user_external_$(app)_elf_end);' >> $@; \
	)

$(BUILTIN_CONFIG_OBJ): $(BUILTIN_CONFIG_C)
	$(CC) $(CFLAGS) -c $< -o $@

$(USER_COMMON_OBJECT): $(USER_DIR)/common/start.S
	@mkdir -p $(dir $@)
	$(CC) $(USER_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/user/builtin/%_c.o: $(USER_DIR)/apps/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/user/external/%_c.o: $(USER_DIR)/apps/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/user/lib/%_c.o: $(USER_DIR)/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/user/lib/libc/%_c.o: $(USER_DIR)/lib/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -fPIC -c $< -o $@

$(USER_LIBC_A): $(USER_LIBC_OBJECTS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(USER_LIBC_OBJECTS)

$(USER_LIBC_SO): $(USER_LIBC_OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) $(USER_SHARED_LDFLAGS) -Wl,-soname,libc.so -o $@ $(USER_LIBC_OBJECTS)
	$(OBJCOPY) --strip-debug $@

$(BUILD_DIR)/user/builtin/simple.elf: $(BUILD_DIR)/user/builtin/simple_c.o
	@mkdir -p $(dir $@)
	$(CC) -nostdlib -nostartfiles -nodefaultlibs -pie -Wl,-e,_start -Wl,-z,max-page-size=0x1000 -o $@ $<

$(BUILD_DIR)/user/builtin/%.elf: $(USER_COMMON_OBJECT) $(BUILD_DIR)/user/builtin/%_c.o $(USER_LIBC_SO) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(USER_DYNAMIC_APP_LDFLAGS) -L$(BUILD_DIR)/user/lib -o $@ $(USER_COMMON_OBJECT) $(BUILD_DIR)/user/builtin/$*_c.o -lc
	$(OBJCOPY) --strip-debug $@

$(BUILD_DIR)/user/external/%.elf: $(USER_COMMON_OBJECT) $(BUILD_DIR)/user/external/%_c.o $(USER_LIBC_SO) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(USER_DYNAMIC_APP_LDFLAGS) -L$(BUILD_DIR)/user/lib -o $@ $(USER_COMMON_OBJECT) $(BUILD_DIR)/user/external/$*_c.o -lc
	$(OBJCOPY) --strip-debug $@

$(BUILD_DIR)/user/external/shared_client.elf: $(USER_COMMON_OBJECT) $(BUILD_DIR)/user/external/shared_client_c.o $(USER_LIBC_SO) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(USER_DYNAMIC_APP_LDFLAGS) -L$(BUILD_DIR)/user/lib -o $@ $(USER_COMMON_OBJECT) $(BUILD_DIR)/user/external/shared_client_c.o -lc
	$(OBJCOPY) --strip-debug $@

$(BUILD_DIR)/user/lib/lib%.so: $(BUILD_DIR)/user/lib/%_c.o
	@mkdir -p $(dir $@)
	$(CC) $(USER_SHARED_LDFLAGS) -Wl,-soname,lib$*.so -o $@ $(BUILD_DIR)/user/lib/$*_c.o
	$(OBJCOPY) --strip-debug $@

$(BUILD_DIR)/user/linker/%_c.o: $(USER_DIR)/linker/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/user/linker/%_s.o: $(USER_DIR)/linker/%.S
	@mkdir -p $(dir $@)
	$(CC) $(USER_ASFLAGS) -c $< -o $@

$(USER_LINKER_SO): $(USER_LINKER_OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) $(USER_SHARED_LDFLAGS) -Wl,-e,_start -o $@ $(USER_LINKER_OBJECTS)
	$(OBJCOPY) --strip-debug $@

$(BUILD_DIR)/user/lib/ld_so_blob.o: $(USER_LINKER_SO)
	@mkdir -p $(dir $@)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$< $@

$(BUILD_DIR)/user/lib/libc_so_blob.o: $(USER_LIBC_SO)
	@mkdir -p $(dir $@)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$< $@

$(BUILD_DIR)/user/builtin/%_elf_blob.o: $(BUILD_DIR)/user/builtin/%.elf
	@mkdir -p $(dir $@)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$< $@

$(BUILD_DIR)/user/lib/lib%_so_blob.o: $(BUILD_DIR)/user/lib/lib%.so
	@mkdir -p $(dir $@)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$< $@

$(BUILD_DIR)/user/external/%_elf_blob.o: $(BUILD_DIR)/user/external/%.elf
	@mkdir -p $(dir $@)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$< $@

clean:
	rm -rf $(BUILD_DIR)

run: $(TARGET_IMG)
	QEMU_BIN=$(QEMU_BIN) KERNEL_IMG=$(TARGET_IMG) \
	bash scripts/run_qemu.sh

run-serial-tcp: $(TARGET_IMG)
	QEMU_BIN=$(QEMU_BIN) KERNEL_IMG=$(TARGET_IMG) \
	bash scripts/run_qemu_serial_tcp.sh

debug: $(TARGET_IMG)
	QEMU_BIN=$(QEMU_BIN) KERNEL_IMG=$(TARGET_IMG) \
	bash scripts/run_qemu_debug.sh

-include $(DEPS)

.PHONY: all clean run run-serial-tcp debug