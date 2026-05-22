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

FONT_BDF ?= src/assets/font.bdf
FONT_WIDTH ?= 8
FONT_HEIGHT ?= 14
FONT_BIN := build/assets/font.bin
FONT_SCRIPT := tools/scripts/gen_font.py

COMMON_CFLAGS := $(GLOBAL_CFLAGS) \
    -DKERNEL_FONT_WIDTH=$(FONT_WIDTH) \
    -DKERNEL_FONT_HEIGHT=$(FONT_HEIGHT) \
    -march=x86-64 \
    -mtune=generic \
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

# QEMU config
QEMU_CPU ?= qemu64
QEMU_MEM ?= 8G
QEMU_SMP ?= 4
QEMU_EXTRA ?=

PYTHON ?= python3
AHCI_DISK_TOOL := tools/scripts/mk_ahci_disk.py
EXT2_DISK_TOOL := tools/scripts/mk_ext2_disk.py

# AHCI test disks.
#
# Enabled by default so `make run` automatically creates and attaches:
#
#   /dev/sda  = MBR-partitioned AHCI disk
#   /dev/sda1
#   /dev/sda2
#
#   /dev/sdb  = GPT-partitioned AHCI disk
#   /dev/sdb1
#   /dev/sdb2
#
# Disable the test disks with:
#
#   make run AHCI_TEST=0
#
AHCI_TEST ?= 1
AHCI_TEST_DISK_SIZE ?= 128M
AHCI_TEST_DISK_DIR := build/testdisks

AHCI_MBR_DISK := $(AHCI_TEST_DISK_DIR)/ahci-mbr.img
AHCI_GPT_DISK := $(AHCI_TEST_DISK_DIR)/ahci-gpt.img
AHCI_EXT2_DISK := $(AHCI_TEST_DISK_DIR)/ahci-ext2.img
AHCI_EXT2_DISK_SIZE ?= 32M
AHCI_TEST_DISK_IMAGES := $(AHCI_MBR_DISK) $(AHCI_GPT_DISK) $(AHCI_EXT2_DISK)

ifeq ($(AHCI_TEST),1)
# Keep QEMU to one AHCI controller. Plain -cdrom on q35 can create a second
# default ICH9 SATA/AHCI controller, which makes interrupt testing noisy.
# Attach the boot ISO as an ATAPI CD on port 5 of the same controller.
QEMU_AHCI_FLAGS := -device ich9-ahci,id=ahci0,bus=pcie.0,addr=0x9 \
    -drive if=none,id=bootiso,file=dist/x86_64/kernel.iso,media=cdrom,readonly=on \
    -device ide-cd,drive=bootiso,bus=ahci0.5,bootindex=0 \
    -drive if=none,id=ahci_mbr,file=$(AHCI_MBR_DISK),format=raw \
    -device ide-hd,drive=ahci_mbr,bus=ahci0.0 \
    -drive if=none,id=ahci_gpt,file=$(AHCI_GPT_DISK),format=raw \
    -device ide-hd,drive=ahci_gpt,bus=ahci0.1 \
    -drive if=none,id=ahci_ext2,file=$(AHCI_EXT2_DISK),format=raw \
    -device ide-hd,drive=ahci_ext2,bus=ahci0.2

QEMU_AHCI_DEPS := $(AHCI_TEST_DISK_IMAGES)
QEMU_BOOT_FLAGS := -boot d
else
# Still use an explicit controller for the boot ISO because QEMUFLAGS uses
# -nodefaults. This disables the AHCI test disks, not the boot CD.
QEMU_AHCI_FLAGS := -device ich9-ahci,id=ahci0,bus=pcie.0,addr=0x9 \
    -drive if=none,id=bootiso,file=dist/x86_64/kernel.iso,media=cdrom,readonly=on \
    -device ide-cd,drive=bootiso,bus=ahci0.5,bootindex=0

QEMU_AHCI_DEPS :=
QEMU_BOOT_FLAGS := -boot d
endif

QEMUFLAGS := -nodefaults \
            -d cpu_reset \
            -machine q35,sata=off \
            -device VGA \
            -D qemu.log \
            -m $(QEMU_MEM) \
            -smp $(QEMU_SMP) \
            -netdev user,id=net0 \
            -netdev user,id=net1 \
            -device pcie-pci-bridge,id=bridge1,bus=pcie.0,addr=0x6 \
            -device e1000,netdev=net1,bus=bridge1,addr=0x1 \
            -device virtio-net-pci,netdev=net0,bus=pcie.0,addr=0x4 \
            -device qemu-xhci,id=xhci,bus=pcie.0,addr=0x5 \
            -device pci-ohci,id=ohci1,bus=pcie.0,addr=0x7 \
            -device pci-ohci,id=ohci2,bus=pcie.0,addr=0x8 \
            -cpu $(QEMU_CPU) \
            $(QEMU_BOOT_FLAGS) \
            -serial stdio \
            $(QEMU_AHCI_FLAGS) \
            $(QEMU_EXTRA)

USER_APPS_DIRS := $(wildcard $(TOP_DIR)/src/user/*/)
USER_APPS := $(filter-out $(TOP_DIR)/src/user/common.mk, $(USER_APPS_DIRS))
USER_APP_SOURCE_FILES := $(shell find $(TOP_DIR)/src/user -path '*/src/*' \( -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.hpp' \) 2>/dev/null)
USER_APP_MAKEFILES := $(foreach dir,$(USER_APPS),$(dir)Makefile) $(TOP_DIR)/src/user/common.mk
USER_APPS_STAMP := build/.user-apps.stamp

MODULE_DIRS := $(wildcard $(TOP_DIR)/src/module/*/)
MODULES := $(filter-out $(TOP_DIR)/src/module/common.mk, $(MODULE_DIRS))
MODULE_SOURCE_FILES := $(shell find $(TOP_DIR)/src/module -path '*/src/*' \( -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.hpp' \) -o -path '*/include/*' \( -name '*.h' -o -name '*.hpp' \) 2>/dev/null)
MODULE_MAKEFILES := $(foreach dir,$(MODULES),$(dir)Makefile) $(TOP_DIR)/src/module/common.mk
MODULES_STAMP := build/.modules.stamp

.PHONY: FORCE newlib
FORCE:

newlib: $(SYSROOT)/lib/libc.a

$(NEWLIB_STAMP): | $(TOOLCHAIN_STAMP) $(SUBMODULE_STAMP)
	@mkdir -p $(NEWLIB_BUILD) $(SYSROOT)
	@printf "$(YELLOW)[NEWLIB]$(NC) Configuring & Compiling...\n"
	$(Q)cd $(NEWLIB_BUILD) && env PATH=$(TOOLCHAIN_DIR)/bin:$$PATH $(NEWLIB_SRC)/configure \
		--target=x86_64-elf \
		--prefix=$(shell dirname $(SYSROOT)) \
		--disable-newlib-supplied-syscalls \
		--disable-nls \
		--enable-newlib-reent-check-verify \
		--enable-newlib-retargetable-locking \
		--quiet \
		--silent \
		CC_FOR_TARGET=$(CC) \
		AS_FOR_TARGET=$(AS) \
		LD_FOR_TARGET=$(LD) \
		RANLIB_FOR_TARGET=$(RANLIB) \
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

$(USER_APPS): newlib | $(TOOLCHAIN_STAMP)
	@printf "$(YELLOW)[BUILD]$(NC) User App: $(notdir $(patsubst %/,%,$@))\n"
	$(Q)$(MAKE) -C $@ all --no-print-directory -s

$(USER_APPS_STAMP): $(USER_APP_SOURCE_FILES) $(USER_APP_MAKEFILES) $(NEWLIB_STAMP) | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	$(Q)$(MAKE) build-user-apps --no-print-directory
	$(Q)touch $@

.PHONY: build-modules $(MODULES)
build-modules: $(MODULES)

$(MODULES): | $(TOOLCHAIN_STAMP)
	@printf "$(YELLOW)[BUILD]$(NC) Module: $(notdir $(patsubst %/,%,$@))\n"
	$(Q)$(MAKE) -C $@ all --no-print-directory -s

$(MODULES_STAMP): $(MODULE_SOURCE_FILES) $(MODULE_MAKEFILES) | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	$(Q)$(MAKE) build-modules --no-print-directory
	$(Q)touch $@

$(INITRAMFS_BIN): $(USER_APPS_STAMP) $(MODULES_STAMP) $(shell find $(INITRAMFS_SRC) -type f -not -path "$(INITRAMFS_SRC)/bin/*" -not -path "$(INITRAMFS_SRC)/lib/modules/*" 2>/dev/null)
	@mkdir -p build $(INITRAMFS_SRC)/bin $(INITRAMFS_SRC)/lib/modules
	@printf "$(YELLOW)[RAMDISK]$(NC) Preparing initramfs...\n"
	$(Q)$(foreach dir,$(USER_APPS),cp $(USER_BINARIES_DIR)/$(shell basename $(dir)) $(INITRAMFS_SRC)/bin/ ;)
	$(Q)$(foreach dir,$(MODULES),cp $(MODULE_BINARIES_DIR)/$(shell basename $(dir)).ko $(INITRAMFS_SRC)/lib/modules/ ;)
	@printf "$(YELLOW)[RAMDISK]$(NC) Building $(INITRAMFS_BIN)\n"
	$(Q)cd $(INITRAMFS_SRC) && find . | cpio -o -H newc > ../$(INITRAMFS_BIN) 2>/dev/null

$(FONT_BIN): $(FONT_BDF) $(FONT_SCRIPT) FORCE
	@mkdir -p $(dir $@)
	@printf "$(BLUE)[FONT]$(NC) Generating $(notdir $@)\n"
	$(Q)test $(FONT_WIDTH) -ge 1 && test $(FONT_WIDTH) -le 32
	$(Q)test $(FONT_HEIGHT) -ge 1 && test $(FONT_HEIGHT) -le 64
	$(Q)$(PYTHON) $(FONT_SCRIPT) $(FONT_BDF) $@ $(FONT_WIDTH) $(FONT_HEIGHT)

build/x86_64/%.asm.o: src/impl/x86_64/%.asm $(FONT_BIN)
	@mkdir -p $(dir $@)
	@printf "$(BLUE)[AS]$(NC) $<\n"
	$(Q)nasm $(NASMFLAGS) $< -o $@

build/x86_64/framebuffer.cpp.o: $(FONT_BIN)

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

.PHONY: ahci-test-disks clean-ahci-test-disks recreate-ahci-test-disks check-host-tools print-qemu

check-host-tools:
	@command -v qemu-system-x86_64 >/dev/null || { echo "missing qemu-system-x86_64"; exit 1; }
	@command -v grub-mkrescue >/dev/null || { echo "missing grub-mkrescue"; exit 1; }
	@command -v $(PYTHON) >/dev/null || { echo "missing $(PYTHON)"; exit 1; }

ahci-test-disks: $(AHCI_TEST_DISK_IMAGES)

recreate-ahci-test-disks: clean-ahci-test-disks ahci-test-disks

$(AHCI_MBR_DISK): Makefile $(AHCI_DISK_TOOL)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[DISK]$(NC) Creating MBR AHCI test disk $@ ($(AHCI_TEST_DISK_SIZE))\n"
	$(Q)$(PYTHON) $(AHCI_DISK_TOOL) mbr $@ $(AHCI_TEST_DISK_SIZE)

$(AHCI_GPT_DISK): Makefile $(AHCI_DISK_TOOL)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[DISK]$(NC) Creating GPT AHCI test disk $@ ($(AHCI_TEST_DISK_SIZE))\n"
	$(Q)$(PYTHON) $(AHCI_DISK_TOOL) gpt $@ $(AHCI_TEST_DISK_SIZE)

$(AHCI_EXT2_DISK): Makefile $(EXT2_DISK_TOOL)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[DISK]$(NC) Creating ext2 AHCI test disk $@ ($(AHCI_EXT2_DISK_SIZE))\n"
	$(Q)$(PYTHON) $(EXT2_DISK_TOOL) $@ $(AHCI_EXT2_DISK_SIZE)

clean-ahci-test-disks:
	@printf "$(RED)[CLEAN]$(NC) Removing AHCI test disks...\n"
	$(Q)rm -rf $(AHCI_TEST_DISK_DIR)

print-qemu: build-x86_64 $(QEMU_AHCI_DEPS)
	@printf '%s\n' 'qemu-system-x86_64 $(QEMUFLAGS)'

.PHONY: run
run: build-x86_64 $(QEMU_AHCI_DEPS)
	@printf "$(GREEN)--- Starting QEMU ---$(NC)\n"
	$(Q)qemu-system-x86_64 $(QEMUFLAGS)

.PHONY: debug
debug: DEBUG=1
debug: build-x86_64 $(QEMU_AHCI_DEPS)
	@printf "$(YELLOW)[DEBUG]$(NC) Starting QEMU in debug mode...\n"
	$(Q)qemu-system-x86_64 $(QEMUFLAGS) -S -s -no-shutdown -no-reboot & \
	QEMU_PID=$$!; \
	gdb dist/x86_64/kernel.bin \
		-ex 'target remote localhost:1234' \
		-ex 'set disassembly-flavor intel'; \
	stty sane; \
	if ps -p $$QEMU_PID > /dev/null; then kill -9 $$QEMU_PID; fi

.PHONY: clean clear
clear: clean

clean:
	@printf "$(RED)[CLEAN]$(NC) Removing root build artifacts...\n"
	$(Q)rm -rf build dist initramfs/bin initramfs/lib/modules

.PHONY: clean-all
clean-all: clean
	@printf "$(RED)[CLEAN]$(NC) Removing toolchain and venv...\n"
	$(Q)rm -rf $(TOOLCHAIN_DIR) $(VENV)

-include $(x86_64_object_files:.o=.d)