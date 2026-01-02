include base.mk

# --- Source Discovery ---
# Kernel ASM sources
x86_64_asm_source_files := $(shell find src/impl/x86_64 -name '*.asm')
x86_64_asm_object_files := $(patsubst src/impl/x86_64/%.asm, build/x86_64/%.asm.o, $(x86_64_asm_source_files))

# Kernel C++ sources
x86_64_cpp_source_files := $(shell find src/impl/x86_64 -name '*.cpp')
x86_64_cpp_object_files := $(patsubst src/impl/x86_64/%.cpp, build/x86_64/%.cpp.o, $(x86_64_cpp_source_files))

# Kernel C sources
x86_64_c_source_files := $(shell find src/impl/x86_64 -name '*.c')
x86_64_c_object_files := $(patsubst src/impl/x86_64/%.c, build/x86_64/%.c.o, $(x86_64_c_source_files))

x86_64_object_files := $(x86_64_asm_object_files) $(x86_64_c_object_files) $(x86_64_cpp_object_files)

# Files for auto-formatting
FORMAT_SOURCES := $(shell find src -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.hpp')

DEBUG ?= 0

TOOLS_ZIP   := $(TOOLS_DIR)/x86_64-elf-tools-linux.zip
TOOLS_STAMP := $(TOOLS_DIR)/.installed
TOOLS_URL   := https://github.com/lordmilko/i686-elf-tools/releases/download/15.2.0/x86_64-elf-tools-linux.zip

FONT_TTF    := src/assets/font.ttf
FONT_BIN    := build/assets/font.bin
FONT_SCRIPT := tools/scripts/gen_font.py

VENV        := .venv
VENV_PYTHON := $(VENV)/bin/python3
VENV_STAMP  := $(VENV)/.installed

QEMUFLAGS := -d int,cpu_reset \
            -D qemu.log \
            -m 1G \
            -smp 4 \
            -cpu qemu64,+pdpe1gb \
            -cdrom dist/x86_64/kernel.iso \
            -serial stdio

# Kernel Specific Flags (Extending GLOBAL_CFLAGS from base.mk)
COMMON_CFLAGS := $(GLOBAL_CFLAGS) \
    -ffreestanding \
    -fno-unwind-tables \
    -fno-asynchronous-unwind-tables \
    -fno-stack-protector \
    -fno-pic \
    -mcmodel=kernel \
    -mno-red-zone \
    -I src/intf \
    -I src/public

NASMFLAGS := -f elf64 -w-zeroing
LDFLAGS   := 

INITRAMFS_SRC := initramfs
INITRAMFS_BIN := build/initramfs.cpio

ifeq ($(DEBUG),1)
    CFLAGS    := $(COMMON_CFLAGS) -O0 -g -DDEBUG
    NASMFLAGS := $(NASMFLAGS) -g -F dwarf
else
    CFLAGS    := $(COMMON_CFLAGS) -O2
    LDFLAGS   := $(LDFLAGS) -s
endif

CXXFLAGS := $(CFLAGS) -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit

# Discovery for sub-projects
USER_APPS_DIRS := $(wildcard src/user/*/)
USER_APPS      := $(filter-out src/user/common.mk, $(USER_APPS_DIRS))

MODULE_DIRS    := $(wildcard src/module/*/)
MODULES        := $(filter-out src/module/common.mk, $(MODULE_DIRS))

BUILD_CONFIG := DEBUG=$(DEBUG)
CONFIG_STAMP := build/.build_config

ifneq ($(shell cat $(CONFIG_STAMP) 2>/dev/null),$(BUILD_CONFIG))
    .PHONY: $(CONFIG_STAMP)
endif

$(CONFIG_STAMP):
	@mkdir -p build
	@echo "$(BUILD_CONFIG)" > $@

.PHONY: all
all: build-x86_64

.PHONY: build-user-apps $(USER_APPS)
build-user-apps: $(USER_APPS)

$(USER_APPS):
	@echo -e "$(YELLOW)[BUILD]$(NC) User App: $(notdir $(patsubst %/,%,$@))"
	$(Q)$(MAKE) -C $@ --no-print-directory -s

.PHONY: build-modules $(MODULES)
build-modules: $(MODULES)

$(MODULES):
	@echo -e "$(YELLOW)[BUILD]$(NC) Module: $(notdir $(patsubst %/,%,$@))"
	$(Q)$(MAKE) -C $@ --no-print-directory -s

$(INITRAMFS_BIN): build-user-apps build-modules $(shell find $(INITRAMFS_SRC) -type f -not -path "$(INITRAMFS_SRC)/bin/*" 2>/dev/null)
	@mkdir -p build $(INITRAMFS_SRC)/bin $(INITRAMFS_SRC)/lib/modules
	@echo -e "$(YELLOW)[RAMDISK]$(NC) Preparing initramfs..."
	@$(foreach dir,$(USER_APPS),cp $(dir)/bin/$(shell basename $(dir)) $(INITRAMFS_SRC)/bin/ ;)
	@$(foreach dir,$(MODULES),cp $(dir)/bin/$(shell basename $(dir)).ko $(INITRAMFS_SRC)/lib/modules/ ;)
	@echo -e "$(YELLOW)[RAMDISK]$(NC) Building $(INITRAMFS_BIN)"
	$(Q)cd $(INITRAMFS_SRC) && find . | cpio -o -H newc > ../$(INITRAMFS_BIN) 2>/dev/null

$(TOOLS_STAMP):
	@mkdir -p $(TOOLS_DIR)
	@echo -e "$(BLUE)[TOOLS]$(NC) Downloading x86_64-elf toolchain..."
	@curl -L $(TOOLS_URL) -o $(TOOLS_ZIP)
	@echo -e "$(BLUE)[TOOLS]$(NC) Extracting..."
	@unzip -o $(TOOLS_ZIP) -d $(TOOLS_DIR)
	@touch $@

$(VENV_STAMP):
	@echo -e "$(BLUE)[VENV]$(NC) Creating Python virtual environment..."
	$(Q)test -d $(VENV) || python3 -m venv $(VENV)
	$(Q)$(VENV_PYTHON) -m pip install Pillow
	@touch $@

$(FONT_BIN): $(FONT_TTF) $(FONT_SCRIPT) | $(VENV_STAMP)
	@mkdir -p $(dir $@)
	@echo -e "$(BLUE)[FONT]$(NC) Generating $(notdir $@)"
	$(Q)$(VENV_PYTHON) $(FONT_SCRIPT) $(FONT_TTF) $@

build/x86_64/%.asm.o: src/impl/x86_64/%.asm $(FONT_BIN) | $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	@echo -e "$(BLUE)[AS]$(NC) $<"
	$(Q)nasm $(NASMFLAGS) $< -o $@

build/x86_64/%.c.o: src/impl/x86_64/%.c | $(TOOLS_STAMP) $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	@echo -e "$(GREEN)[CC]$(NC) $<"
	$(Q)$(CC) -c $(CFLAGS) $< -o $@

build/x86_64/%.cpp.o: src/impl/x86_64/%.cpp | $(TOOLS_STAMP) $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	@echo -e "$(GREEN)[CXX]$(NC) $<"
	$(Q)$(CXX) -c $(CXXFLAGS) $< -o $@

.PHONY: build-x86_64
build-x86_64: $(TOOLS_STAMP) $(x86_64_object_files) $(INITRAMFS_BIN)
	@mkdir -p dist/x86_64
	@echo -e "$(BLUE)[LD]$(NC) dist/x86_64/kernel.bin"
	$(Q)$(LD) -n $(LDFLAGS) -o dist/x86_64/kernel.bin -T targets/x86_64/linker.ld $(x86_64_object_files)
	@mkdir -p targets/x86_64/iso/boot
	@cp dist/x86_64/kernel.bin targets/x86_64/iso/boot/kernel.bin
	@cp $(INITRAMFS_BIN) targets/x86_64/iso/boot/initramfs.cpio
	@echo -e "$(YELLOW)[ISO]$(NC) Generating dist/x86_64/kernel.iso"
	$(Q)grub-mkrescue /usr/lib/grub/i386-pc -o dist/x86_64/kernel.iso targets/x86_64/iso 2>/dev/null

.PHONY: run
run: build-x86_64
	@echo -e "$(GREEN)--- Starting QEMU ---$(NC)"
	$(Q)qemu-system-x86_64 $(QEMUFLAGS)

.PHONY: debug
debug: build-x86_64
	@echo -e "$(YELLOW)[DEBUG]$(NC) Starting QEMU in debug mode..."
	$(Q)qemu-system-x86_64 $(QEMUFLAGS) -S -s -no-shutdown -no-reboot & \
	QEMU_PID=$$!; \
	gdb dist/x86_64/kernel.bin \
        -ex 'target remote localhost:1234' \
        -ex 'set disassembly-flavor intel'; \
	stty sane; \
	if ps -p $$QEMU_PID > /dev/null; then kill -9 $$QEMU_PID; fi

.PHONY: format
format:
	@echo -e "$(BLUE)[FORMAT]$(NC) Formatting C/C++ files..."
	$(Q)clang-format -i $(FORMAT_SOURCES)

.PHONY: check-format
check-format:
	@echo -e "$(BLUE)[FORMAT]$(NC) Checking formatting..."
	$(Q)clang-format --dry-run --Werror $(FORMAT_SOURCES)

.PHONY: clean
clean:
	@echo -e "$(RED)[CLEAN]$(NC) Removing root build artifacts..."
	$(Q)rm -rf build dist initramfs/bin initramfs/lib/modules
	@for dir in $(USER_APPS) $(MODULES); do \
		echo -e "$(RED)[CLEAN]$(NC) $$(basename $$dir)"; \
		$(MAKE) -C $$dir --no-print-directory -s clean; \
	done

.PHONY: clean-all
clean-all: clean
	@echo -e "$(RED)[CLEAN]$(NC) Removing tools and venv..."
	$(Q)rm -rf tools .venv

.PHONY: doc
doc:
	@echo -e "$(BLUE)[DOC]$(NC) Generating documentation..."
	$(Q)doxygen Doxyfile

.PHONY: bear
bear:
	@echo -e "$(BLUE)[BEAR]$(NC) Generating compile_commands.json..."
	$(Q)bear -- make clean build-x86_64

-include $(x86_64_object_files:.o=.d)