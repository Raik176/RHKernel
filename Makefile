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

KERNEL_BIN := dist/x86_64/kernel.bin
KERNEL_ISO := dist/x86_64/kernel.iso
KERNEL_BUILD_CFG := build/.kernel-build.cfg
ISO_SRC_DIR := targets/x86_64/iso
ISO_BUILD_DIR := build/x86_64/iso
ISO_CFG_FILES := $(shell find $(TOP_DIR)/$(ISO_SRC_DIR) -type f ! -name 'kernel.bin' ! -name 'initramfs.cpio' 2>/dev/null)
ASM_INCLUDE_FILES := $(shell find $(TOP_DIR)/src/assets -name '*.inc' 2>/dev/null)

DEBUG ?= 0

FONT_BDF ?= src/assets/font.bdf
FONT_WIDTH ?= 8
FONT_HEIGHT ?= 14
FONT_BIN := build/assets/font.bin
FONT_CFG := build/assets/font.cfg
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

NASMFLAGS := -f elf64 -w-zeroing -I$(TOP_DIR)/
LDFLAGS := $(GLOBAL_LDFLAGS)

INITRAMFS_SRC := initramfs
INITRAMFS_STAGE_DIR := build/initramfs-root
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
FAT_DISK_TOOL := tools/scripts/mk_fat_disk.py

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
FAT12_TEST_DISK := $(AHCI_TEST_DISK_DIR)/fat12.img
FAT16_TEST_DISK := $(AHCI_TEST_DISK_DIR)/fat16.img
FAT32_TEST_DISK := $(AHCI_TEST_DISK_DIR)/fat32.img
FAT12_TEST_DISK_SIZE ?= 1440K
FAT16_TEST_DISK_SIZE ?= 16M
FAT32_TEST_DISK_SIZE ?= 64M
FAT_TEST_DISK_IMAGES := $(FAT12_TEST_DISK) $(FAT16_TEST_DISK) $(FAT32_TEST_DISK)
AHCI_TEST_DISK_IMAGES := $(AHCI_MBR_DISK) $(AHCI_GPT_DISK) $(AHCI_EXT2_DISK) $(FAT_TEST_DISK_IMAGES)

ifeq ($(AHCI_TEST),1)
# Use explicit AHCI controllers only. Plain -cdrom on q35 can create an
# implicit storage controller, which makes interrupt testing noisy.
# Keep the boot ISO as an ATAPI CD on ahci0 port 5.
QEMU_AHCI_FLAGS := -device ich9-ahci,id=ahci0,bus=pcie.0,addr=0x9 \
    -drive if=none,id=bootiso,file=dist/x86_64/kernel.iso,media=cdrom,readonly=on \
    -device ide-cd,drive=bootiso,bus=ahci0.5,bootindex=0 \
    -drive if=none,id=ahci_mbr,file=$(AHCI_MBR_DISK),format=raw \
    -device ide-hd,drive=ahci_mbr,bus=ahci0.0 \
    -drive if=none,id=ahci_gpt,file=$(AHCI_GPT_DISK),format=raw \
    -device ide-hd,drive=ahci_gpt,bus=ahci0.1 \
    -drive if=none,id=ahci_ext2,file=$(AHCI_EXT2_DISK),format=raw \
    -device ide-hd,drive=ahci_ext2,bus=ahci0.2 \
    -device ich9-ahci,id=ahci1,bus=pcie.0,addr=0xa \
    -drive if=none,id=fat12,file=$(FAT12_TEST_DISK),format=raw \
    -device ide-hd,drive=fat12,bus=ahci1.0 \
    -drive if=none,id=fat16,file=$(FAT16_TEST_DISK),format=raw \
    -device ide-hd,drive=fat16,bus=ahci1.1 \
    -drive if=none,id=fat32,file=$(FAT32_TEST_DISK),format=raw \
    -device ide-hd,drive=fat32,bus=ahci1.2

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
            -d cpu_reset,guest_errors \
            -no-reboot -no-shutdown \
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
            -debugcon file:debugcon.log \
            $(QEMU_EXTRA)

USER_APP_MAKEFILE_FILES := $(shell find $(TOP_DIR)/src/user -mindepth 2 -name Makefile 2>/dev/null)
USER_APPS := $(patsubst %Makefile,%,$(USER_APP_MAKEFILE_FILES))
USER_APP_SOURCE_FILES := $(shell find $(TOP_DIR)/src/user \( -path '*/src/*' -o -path '*/include/*' \) \( -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.hpp' \) 2>/dev/null)
USER_APP_PUBLIC_HEADERS := $(shell find $(TOP_DIR)/src/public \( -name '*.h' -o -name '*.hpp' \) 2>/dev/null)
USER_APP_MAKEFILES := $(USER_APP_MAKEFILE_FILES) $(TOP_DIR)/src/user/common.mk $(TOP_DIR)/src/user/linker.ld $(TOP_DIR)/base.mk
USER_APPS_STAMP := build/.user-apps.stamp

MODULE_DIRS := $(wildcard $(TOP_DIR)/src/module/*/)
MODULES := $(filter-out $(TOP_DIR)/src/module/common.mk, $(MODULE_DIRS))
MODULE_SOURCE_FILES := $(shell find $(TOP_DIR)/src/module \( -path '*/src/*' -o -path '*/include/*' \) \( -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.hpp' \) 2>/dev/null)
MODULE_PUBLIC_HEADERS := $(shell find $(TOP_DIR)/src/public \( -name '*.h' -o -name '*.hpp' \) 2>/dev/null)
MODULE_MAKEFILES := $(foreach dir,$(MODULES),$(dir)Makefile) $(TOP_DIR)/src/module/common.mk $(TOP_DIR)/base.mk
MODULES_STAMP := build/.modules.stamp

.PHONY: FORCE newlib
FORCE:

newlib: $(SYSROOT)/lib/libc.a

$(NEWLIB_CONFIG): FORCE | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	$(Q){ \
		printf 'NEWLIB_SRC=%s\n' '$(NEWLIB_SRC)'; \
		printf 'NEWLIB_BUILD=%s\n' '$(NEWLIB_BUILD)'; \
		printf 'NEWLIB_PREFIX=%s\n' '$(NEWLIB_PREFIX)'; \
		printf 'SYSROOT=%s\n' '$(SYSROOT)'; \
		printf 'CC=%s\n' '$(CC)'; \
		printf 'CXX=%s\n' '$(CXX)'; \
		printf 'AS=%s\n' '$(AS)'; \
		printf 'LD=%s\n' '$(LD)'; \
		printf 'AR=%s\n' '$(AR)'; \
		printf 'RANLIB=%s\n' '$(RANLIB)'; \
		printf 'NM=%s\n' '$(NM)'; \
		printf 'OBJCOPY=%s\n' '$(OBJCOPY)'; \
		printf 'OBJDUMP=%s\n' '$(OBJDUMP)'; \
		printf 'READELF=%s\n' '$(READELF)'; \
		printf 'STRIP=%s\n' '$(STRIP)'; \
		printf 'NEWLIB_TARGET_CFLAGS=%s\n' '$(NEWLIB_TARGET_CFLAGS)'; \
		printf 'CONFIGURE_FLAGS=%s\n' '--target=x86_64-elf --prefix=$(NEWLIB_PREFIX) --disable-newlib-supplied-syscalls --disable-nls --enable-newlib-reent-check-verify --enable-newlib-retargetable-locking'; \
	} > $@.tmp
	$(Q)if test -r $@ && cmp -s $@ $@.tmp; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(NEWLIB_STAMP): $(NEWLIB_CONFIG) Makefile base.mk | $(TOOLCHAIN_STAMP) $(SUBMODULE_STAMP)
	$(Q)rm -rf $(NEWLIB_BUILD) $(SYSROOT)
	@mkdir -p $(NEWLIB_BUILD) $(SYSROOT)
	@printf "$(YELLOW)[NEWLIB]$(NC) Configuring & Compiling...\n"
	$(Q)cd $(NEWLIB_BUILD) && env PATH="$(TOOLCHAIN_DIR)/bin:$$PATH" $(NEWLIB_SRC)/configure \
		--target=x86_64-elf \
		--prefix=$(NEWLIB_PREFIX) \
		--disable-newlib-supplied-syscalls \
		--disable-nls \
		--enable-newlib-reent-check-verify \
		--enable-newlib-retargetable-locking \
		--quiet \
		--silent \
		CC_FOR_TARGET="$(CC)" \
		CXX_FOR_TARGET="$(CXX)" \
		AS_FOR_TARGET="$(AS)" \
		LD_FOR_TARGET="$(LD)" \
		AR_FOR_TARGET="$(AR)" \
		RANLIB_FOR_TARGET="$(RANLIB)" \
		NM_FOR_TARGET="$(NM)" \
		OBJCOPY_FOR_TARGET="$(OBJCOPY)" \
		OBJDUMP_FOR_TARGET="$(OBJDUMP)" \
		READELF_FOR_TARGET="$(READELF)" \
		STRIP_FOR_TARGET="$(STRIP)" \
		CFLAGS_FOR_TARGET="$(NEWLIB_TARGET_CFLAGS)" \
		CXXFLAGS_FOR_TARGET="$(NEWLIB_TARGET_CFLAGS)"
	+$(Q)$(MAKE) -C $(NEWLIB_BUILD) --quiet
	+$(Q)$(MAKE) -C $(NEWLIB_BUILD) install
	@printf "$(YELLOW)[NEWLIB]$(NC) Done.\n"
	@touch $@

$(SYSROOT)/lib/libc.a: $(NEWLIB_STAMP)
	$(Q)test -r $@
	$(Q)$(AR) t $@ >/dev/null

$(NEWLIB_STUB_OBJ_FILES): | $(NEWLIB_STAMP)

.PHONY: build-user-apps $(USER_APPS)
build-user-apps: $(USER_APPS)

$(USER_APPS): newlib $(NEWLIB_CRT0) $(NEWLIB_STUB_LIB_OBJ_FILES) | $(TOOLCHAIN_STAMP)
	@printf "$(YELLOW)[BUILD]$(NC) User App: $(notdir $(patsubst %/,%,$@))\n"
	+$(Q)$(MAKE) -C $@ all --no-print-directory -s

$(USER_APPS_STAMP): $(USER_APP_SOURCE_FILES) $(USER_APP_PUBLIC_HEADERS) $(USER_APP_MAKEFILES) $(NEWLIB_STAMP) $(NEWLIB_CRT0) $(NEWLIB_STUB_LIB_OBJ_FILES) | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	+$(Q)$(MAKE) build-user-apps --no-print-directory
	$(Q)touch $@

.PHONY: build-modules $(MODULES)
build-modules: $(MODULES)

$(MODULES): | $(TOOLCHAIN_STAMP)
	@printf "$(YELLOW)[BUILD]$(NC) Module: $(notdir $(patsubst %/,%,$@))\n"
	+$(Q)$(MAKE) -C $@ all --no-print-directory -s

$(MODULES_STAMP): $(MODULE_SOURCE_FILES) $(MODULE_PUBLIC_HEADERS) $(MODULE_MAKEFILES) | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	+$(Q)$(MAKE) build-modules --no-print-directory
	$(Q)touch $@

$(INITRAMFS_BIN): $(USER_APPS_STAMP) $(MODULES_STAMP) Makefile $(shell find $(INITRAMFS_SRC) -type f -not -path "$(INITRAMFS_SRC)/bin/*" -not -path "$(INITRAMFS_SRC)/lib/modules/*" 2>/dev/null)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[RAMDISK]$(NC) Preparing initramfs...\n"
	$(Q)rm -rf $(INITRAMFS_STAGE_DIR)
	$(Q)mkdir -p $(INITRAMFS_STAGE_DIR)/bin $(INITRAMFS_STAGE_DIR)/lib/modules
	$(Q)if test -d $(INITRAMFS_SRC); then cp -a $(INITRAMFS_SRC)/. $(INITRAMFS_STAGE_DIR)/; fi
	$(Q)rm -rf $(INITRAMFS_STAGE_DIR)/bin $(INITRAMFS_STAGE_DIR)/lib/modules
	$(Q)mkdir -p $(INITRAMFS_STAGE_DIR)/bin $(INITRAMFS_STAGE_DIR)/lib/modules
	$(Q)for dir in $(USER_APPS); do \
		rel=$${dir#$(TOP_DIR)/src/user/}; \
		rel=$${rel%/}; \
		mkdir -p $(INITRAMFS_STAGE_DIR)/bin/$$(dirname "$$rel"); \
		cp $(USER_BINARIES_DIR)/$$rel $(INITRAMFS_STAGE_DIR)/bin/$$rel; \
	done
	$(Q)$(foreach dir,$(MODULES),cp $(MODULE_BINARIES_DIR)/$(shell basename $(dir)).ko $(INITRAMFS_STAGE_DIR)/lib/modules/ ;)
	@printf "$(YELLOW)[RAMDISK]$(NC) Building $(INITRAMFS_BIN)\n"
	$(Q)cd $(INITRAMFS_STAGE_DIR) && find . -print | LC_ALL=C sort | cpio -o -H newc > $(TOP_DIR)/$(INITRAMFS_BIN).tmp 2>/dev/null
	$(Q)mv $(INITRAMFS_BIN).tmp $(INITRAMFS_BIN)

$(KERNEL_BUILD_CFG): FORCE
	@mkdir -p $(dir $@)
	$(Q){ \
		printf 'DEBUG=%s\n' '$(DEBUG)'; \
		printf 'CFLAGS=%s\n' '$(CFLAGS)'; \
		printf 'CXXFLAGS=%s\n' '$(CXXFLAGS)'; \
		printf 'NASMFLAGS=%s\n' '$(NASMFLAGS)'; \
		printf 'LDFLAGS=%s\n' '$(LDFLAGS)'; \
	} > $@.tmp
	$(Q)if test -r $@ && cmp -s $@ $@.tmp; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(FONT_CFG): FORCE
	@mkdir -p $(dir $@)
	$(Q){ \
		printf 'FONT_BDF=%s\n' '$(FONT_BDF)'; \
		printf 'FONT_WIDTH=%s\n' '$(FONT_WIDTH)'; \
		printf 'FONT_HEIGHT=%s\n' '$(FONT_HEIGHT)'; \
	} > $@.tmp
	$(Q)if test -r $@ && cmp -s $@ $@.tmp; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(FONT_BIN): $(FONT_BDF) $(FONT_SCRIPT) $(FONT_CFG)
	@mkdir -p $(dir $@)
	@printf "$(BLUE)[FONT]$(NC) Generating $(notdir $@)\n"
	$(Q)test $(FONT_WIDTH) -ge 1 && test $(FONT_WIDTH) -le 32
	$(Q)test $(FONT_HEIGHT) -ge 1 && test $(FONT_HEIGHT) -le 64
	$(Q)$(PYTHON) $(FONT_SCRIPT) $(FONT_BDF) $@.tmp $(FONT_WIDTH) $(FONT_HEIGHT)
	$(Q)if test -r $@ && cmp -s $@ $@.tmp; then rm -f $@.tmp; else mv $@.tmp $@; fi

build/x86_64/%.asm.o: src/impl/x86_64/%.asm Makefile base.mk $(KERNEL_BUILD_CFG) $(ASM_INCLUDE_FILES) $(FONT_BIN)
	@mkdir -p $(dir $@)
	@printf "$(BLUE)[AS]$(NC) $<\n"
	$(Q)nasm $(NASMFLAGS) $(NASMDEPFLAGS) $< -o $@

build/x86_64/framebuffer.cpp.o: $(FONT_BIN)

build/x86_64/%.c.o: src/impl/x86_64/%.c Makefile base.mk $(KERNEL_BUILD_CFG) | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	@printf "$(GREEN)[CC]$(NC) $<\n"
	$(Q)$(CC) $(DEPFLAGS) -c $(CFLAGS) $< -o $@

build/x86_64/%.cpp.o: src/impl/x86_64/%.cpp Makefile base.mk $(KERNEL_BUILD_CFG) | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	@printf "$(GREEN)[CXX]$(NC) $<\n"
	$(Q)$(CXX) $(DEPFLAGS) -c $(CXXFLAGS) $< -o $@

.PHONY: build-x86_64
build-x86_64: $(SYSROOT)/lib/libc.a $(KERNEL_ISO)

$(KERNEL_BIN): $(TOOLCHAIN_STAMP) $(x86_64_object_files) targets/x86_64/linker.ld Makefile base.mk $(KERNEL_BUILD_CFG)
	@mkdir -p $(dir $@)
	@printf "$(BLUE)[LD]$(NC) $@\n"
	$(Q)$(CC) $(CFLAGS) -nostdlib -o $@.tmp \
        -T targets/x86_64/linker.ld \
        -Wl,-n $(foreach flag,$(LDFLAGS),-Wl$\,$(flag)) \
        $(x86_64_object_files)
	$(Q)mv $@.tmp $@

$(KERNEL_ISO): $(KERNEL_BIN) $(INITRAMFS_BIN) $(ISO_CFG_FILES) Makefile
	$(Q)rm -rf $(ISO_BUILD_DIR)
	$(Q)mkdir -p $(ISO_BUILD_DIR)
	$(Q)cp -a $(ISO_SRC_DIR)/. $(ISO_BUILD_DIR)/
	$(Q)rm -f $(ISO_BUILD_DIR)/boot/kernel.bin $(ISO_BUILD_DIR)/boot/initramfs.cpio
	$(Q)mkdir -p $(ISO_BUILD_DIR)/boot
	$(Q)cp $(KERNEL_BIN) $(ISO_BUILD_DIR)/boot/kernel.bin
	$(Q)cp $(INITRAMFS_BIN) $(ISO_BUILD_DIR)/boot/initramfs.cpio
	@printf "$(YELLOW)[ISO]$(NC) Generating $@\n"
	$(Q)mkdir -p $(dir $@)
	$(Q)grub-mkrescue --compress=xz /usr/lib/grub/i386-pc \
		-o $@.tmp $(ISO_BUILD_DIR) 2>/dev/null
	$(Q)mv $@.tmp $@

.PHONY: ahci-test-disks fat-test-disks clean-ahci-test-disks recreate-ahci-test-disks recreate-fat-test-disks check-host-tools print-qemu

check-host-tools:
	@command -v qemu-system-x86_64 >/dev/null || { echo "missing qemu-system-x86_64"; exit 1; }
	@command -v grub-mkrescue >/dev/null || { echo "missing grub-mkrescue"; exit 1; }
	@command -v $(PYTHON) >/dev/null || { echo "missing $(PYTHON)"; exit 1; }

ahci-test-disks: $(AHCI_TEST_DISK_IMAGES)

fat-test-disks: $(FAT_TEST_DISK_IMAGES)

recreate-ahci-test-disks: clean-ahci-test-disks ahci-test-disks

recreate-fat-test-disks:
	$(Q)rm -f $(FAT_TEST_DISK_IMAGES)
	+$(Q)$(MAKE) fat-test-disks --no-print-directory

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

$(FAT12_TEST_DISK): Makefile $(FAT_DISK_TOOL)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[DISK]$(NC) Creating FAT12 test disk $@ ($(FAT12_TEST_DISK_SIZE))\n"
	$(Q)$(PYTHON) $(FAT_DISK_TOOL) fat12 $@ $(FAT12_TEST_DISK_SIZE)

$(FAT16_TEST_DISK): Makefile $(FAT_DISK_TOOL)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[DISK]$(NC) Creating FAT16 test disk $@ ($(FAT16_TEST_DISK_SIZE))\n"
	$(Q)$(PYTHON) $(FAT_DISK_TOOL) fat16 $@ $(FAT16_TEST_DISK_SIZE)

$(FAT32_TEST_DISK): Makefile $(FAT_DISK_TOOL)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[DISK]$(NC) Creating FAT32 test disk $@ ($(FAT32_TEST_DISK_SIZE))\n"
	$(Q)$(PYTHON) $(FAT_DISK_TOOL) fat32 $@ $(FAT32_TEST_DISK_SIZE)

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
	$(Q)rm -rf build dist

.PHONY: clean-all
clean-all: clean
	@printf "$(RED)[CLEAN]$(NC) Removing cached host tools...\n"
	$(Q)rm -rf $(TOOL_CACHE_DIR)

-include $(x86_64_object_files:.o=.d)