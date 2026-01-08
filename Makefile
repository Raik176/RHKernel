include base.mk

x86_64_asm_source_files := $(shell find $(TOP_DIR)/src/impl/x86_64 -name '*.asm' 2>/dev/null)
x86_64_asm_object_files := $(patsubst $(TOP_DIR)/src/impl/x86_64/%, build/x86_64/%.o, $(x86_64_asm_source_files))

x86_64_cpp_source_files := $(shell find $(TOP_DIR)/src/impl/x86_64 -name '*.cpp' 2>/dev/null)
x86_64_cpp_object_files := $(patsubst $(TOP_DIR)/src/impl/x86_64/%.cpp, build/x86_64/%.cpp.o, $(x86_64_cpp_source_files))

x86_64_c_source_files := $(shell find $(TOP_DIR)/src/impl/x86_64 -name '*.c' 2>/dev/null)
x86_64_c_object_files := $(patsubst $(TOP_DIR)/src/impl/x86_64/%.c, build/x86_64/%.c.o, $(x86_64_c_source_files))

x86_64_object_files := \
    $(x86_64_asm_object_files) \
    $(x86_64_c_object_files) \
    $(x86_64_cpp_object_files)

FORMAT_SOURCES := $(shell find $(TOP_DIR)/src -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.hpp' 2>/dev/null)

DEBUG ?= 0

FONT_TTF := src/assets/font.ttf
FONT_BIN := build/assets/font.bin
FONT_SCRIPT := tools/scripts/gen_font.py

COMMON_CFLAGS := $(GLOBAL_CFLAGS) \
    -ffreestanding \
    -fno-unwind-tables \
    -fno-asynchronous-unwind-tables \
    -fno-stack-protector \
    -fno-pic \
	-fno-builtin \
    -mcmodel=kernel \
    -mno-red-zone \
    -I src/intf \
    -I src/public

NASMFLAGS := -f elf64 -w-zeroing
LDFLAGS := $(GLOBAL_LDFLAGS)

INITRAMFS_SRC := initramfs
INITRAMFS_BIN := build/initramfs.cpio

ifeq ($(DEBUG),1)
    CFLAGS    := $(COMMON_CFLAGS) -O0 -g -DDEBUG
    NASMFLAGS += -g -F dwarf
else
    CFLAGS    := $(COMMON_CFLAGS) -O2
    LDFLAGS   += -s
endif

CXXFLAGS := $(CFLAGS) \
    -fno-exceptions \
    -fno-rtti \
    -fno-threadsafe-statics \
    -fno-use-cxa-atexit

#-device usb-kbd,bus=ohci1.0
QEMUFLAGS := -d int,cpu_reset \
            -machine q35 \
            -D qemu.log \
            -m 1G \
            -smp 4 \
            -netdev user,id=net0 \
            -netdev user,id=net1 \
            -device pcie-pci-bridge,id=bridge1,bus=pcie.0,addr=0x6 \
            -device e1000,netdev=net1,bus=bridge1,addr=0x1 \
            -device virtio-net-pci,netdev=net0,bus=pcie.0,addr=0x4 \
            -device qemu-xhci,id=xhci,bus=pcie.0,addr=0x5 \
            -device pci-ohci,id=ohci1,bus=pcie.0,addr=0x7 \
            -device pci-ohci,id=ohci2,bus=pcie.0,addr=0x8 \
            -cpu qemu64,+pdpe1gb \
            -cdrom dist/x86_64/kernel.iso \
            -serial stdio

USER_APPS_DIRS := $(wildcard $(TOP_DIR)/src/user/*/)
USER_APPS := $(filter-out $(TOP_DIR)/src/user/common.mk, $(USER_APPS_DIRS))

MODULE_DIRS := $(wildcard $(TOP_DIR)/src/module/*/)
MODULES := $(filter-out $(TOP_DIR)/src/module/common.mk, $(MODULE_DIRS))

.PHONY: newlib
newlib: $(SYSROOT)/lib/libc.a

$(NEWLIB_STAMP): | $(TOOLCHAIN_STAMP) $(SUBMODULE_STAMP)
	@mkdir -p $(NEWLIB_BUILD) $(SYSROOT)
	@printf "$(YELLOW)[NEWLIB]$(NC) Configuring & Compiling...\n"
	$(Q)cd $(NEWLIB_BUILD) && $(NEWLIB_SRC)/configure \
		--target=x86_64-elf \
		--prefix=$(shell dirname $(SYSROOT)) \
		--disable-newlib-supplied-syscalls \
		--disable-nls \
		--enable-newlib-reent-check-verify \
		--enable-newlib-retargetable-locking \
		--quiet \
		--silent \
		CC_FOR_TARGET=$(CC) \
		AS_FOR_TARGET=$(AS)
		LD_FOR_TARGET=$(LD) \
		RANLIB_FOR_TARGET=$(RANLIB)
		AR_FOR_TARGET=$(AR)
	$(Q)$(MAKE) -C $(NEWLIB_BUILD) -j$(shell nproc) --quiet
	$(Q)$(MAKE) -C $(NEWLIB_BUILD) install
	@printf "$(YELLOW)[NEWLIB]$(NC) Done.\n"
	@touch $@

$(SYSROOT)/lib/libc.a: $(NEWLIB_STUB_OBJ_FILES)
	@mkdir -p $(dir $(NEWLIB_STUB_OBJ_FILES))
	$(Q)$(AR) rcs $@ $(NEWLIB_STUB_OBJ_FILES)

build/newlib/%.o: src/newlib/%.c $(TOOLCHAIN_STAMP) $(NEWLIB_STAMP)
	$(Q)$(CC) --sysroot=$(abspath $(SYSROOT)) \
		-isystem $(abspath $(SYSROOT)/include) \
		-nostdlib -c $< -lc -o $@

.PHONY: build-user-apps $(USER_APPS)
build-user-apps: $(USER_APPS)

$(USER_APPS): newlib
	@printf "$(YELLOW)[BUILD]$(NC) User App: $(notdir $(patsubst %/,%,$@))\n"
	$(Q)$(MAKE) -C $@ all --no-print-directory -s

.PHONY: build-modules $(MODULES)
build-modules: $(MODULES)

$(MODULES):
	@printf "$(YELLOW)[BUILD]$(NC) Module: $(notdir $(patsubst %/,%,$@))\n"
	$(Q)$(MAKE) -C $@ all --no-print-directory -s

$(INITRAMFS_BIN): build-user-apps build-modules $(shell find $(INITRAMFS_SRC) -type f -not -path "$(INITRAMFS_SRC)/bin/*" 2>/dev/null)
	@mkdir -p build $(INITRAMFS_SRC)/bin $(INITRAMFS_SRC)/lib/modules
	@printf "$(YELLOW)[RAMDISK]$(NC) Preparing initramfs...\n"
	$(Q)$(foreach dir,$(USER_APPS),cp $(USER_BINARIES_DIR)/$(shell basename $(dir)) $(INITRAMFS_SRC)/bin/ ;)
	$(Q)$(foreach dir,$(MODULES),cp $(MODULE_BINARIES_DIR)/$(shell basename $(dir)).ko $(INITRAMFS_SRC)/lib/modules/ ;)
	@printf "$(YELLOW)[RAMDISK]$(NC) Building $(INITRAMFS_BIN)\n"
	$(Q)cd $(INITRAMFS_SRC) && find . | cpio -o -H newc > ../$(INITRAMFS_BIN) 2>/dev/null

$(FONT_BIN): $(FONT_TTF) $(FONT_SCRIPT) | $(VENV_STAMP)
	@mkdir -p $(dir $@)
	@printf "$(BLUE)[FONT]$(NC) Generating $(notdir $@)\n"
	$(Q)$(VENV_PYTHON) $(FONT_SCRIPT) $(FONT_TTF) $@

build/x86_64/%.asm.o: src/impl/x86_64/%.asm $(FONT_BIN)
	@mkdir -p $(dir $@)
	@printf "$(BLUE)[AS]$(NC) $<\n"
	$(Q)nasm $(NASMFLAGS) $< -o $@

build/x86_64/%.c.o: src/impl/x86_64/%.c | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	@printf "$(GREEN)[CC]$(NC) $<\n"
	$(Q)$(CC) -c $(CFLAGS) $< -o $@

build/x86_64/%.cpp.o: src/impl/x86_64/%.cpp | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	@printf "$(GREEN)[CXX]$(NC) $<\n"
	$(Q)$(CXX) -c $(CXXFLAGS) $< -o $@

.PHONY: build-x86_64
build-x86_64: $(TOOLCHAIN_STAMP) $(x86_64_object_files) $(INITRAMFS_BIN)
	@mkdir -p dist/x86_64
	@printf "$(BLUE)[LD]$(NC) dist/x86_64/kernel.bin\n"
	$(Q)$(CC) $(CFLAGS) -nostdlib -o dist/x86_64/kernel.bin \
        -T targets/x86_64/linker.ld \
        -Wl,-n $(foreach flag,$(LDFLAGS),-Wl$\,$(flag)) \
        $(x86_64_object_files)
	$(Q)mkdir -p targets/x86_64/iso/boot
	$(Q)cp dist/x86_64/kernel.bin targets/x86_64/iso/boot/kernel.bin
	$(Q)cp $(INITRAMFS_BIN) targets/x86_64/iso/boot/initramfs.cpio
	@printf "$(YELLOW)[ISO]$(NC) Generating dist/x86_64/kernel.iso\n"
	$(Q)grub-mkrescue /usr/lib/grub/i386-pc \
		-o dist/x86_64/kernel.iso targets/x86_64/iso 2>/dev/null

.PHONY: run
run: build-x86_64
	@printf "$(GREEN)--- Starting QEMU ---$(NC)\n"
	$(Q)qemu-system-x86_64 $(QEMUFLAGS)

.PHONY: debug
debug: build-x86_64
	@printf "$(YELLOW)[DEBUG]$(NC) Starting QEMU in debug mode...\n"
	$(Q)qemu-system-x86_64 $(QEMUFLAGS) -S -s -no-shutdown -no-reboot & \
	QEMU_PID=$$!; \
	gdb dist/x86_64/kernel.bin \
		-ex 'target remote localhost:1234' \
		-ex 'set disassembly-flavor intel'; \
	stty sane; \
	if ps -p $$QEMU_PID > /dev/null; then kill -9 $$QEMU_PID; fi

.PHONY: clean
clean:
	@printf "$(RED)[CLEAN]$(NC) Removing root build artifacts...\n"
	$(Q)rm -rf build dist initramfs/bin initramfs/lib/modules

.PHONY: clean-all
clean-all: clean
	@printf "$(RED)[CLEAN]$(NC) Removing toolchain and venv...\n"
	$(Q)rm -rf $(TOOLCHAIN_DIR) $(VENV)

-include $(x86_64_object_files:.o=.d)
