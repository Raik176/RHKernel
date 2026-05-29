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
    $(KERNEL_CODEGEN_CFLAGS) \
    -DKERNEL_FONT_WIDTH=$(FONT_WIDTH) \
    -DKERNEL_FONT_HEIGHT=$(FONT_HEIGHT) \
    -march=x86-64 \
    -mtune=generic \
    -ffreestanding \
    -fno-unwind-tables \
    -fno-asynchronous-unwind-tables \
    -fstack-protector-strong \
    -mstack-protector-guard=global \
    -fno-pic \
	-fno-builtin \
    -mcmodel=kernel \
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
    CFLAGS    := $(COMMON_CFLAGS) -O2 -fomit-frame-pointer -fno-plt -fno-semantic-interposition
    LDFLAGS   += -s
endif

CXXFLAGS := $(CFLAGS) \
    -fno-exceptions \
    -fno-rtti \
    -fno-threadsafe-statics \
    -fno-use-cxa-atexit

QEMU_MEM ?= 8G
QEMU_SMP ?= 4
# host or max
QEMU_CPU ?= max
QEMU_EXTRA ?=

ifeq ($(QEMU_CPU),host)
    QEMU_ACCEL_FLAGS := -enable-kvm
else
    QEMU_ACCEL_FLAGS :=
endif

PYTHON ?= python3
AHCI_DISK_TOOL := tools/scripts/mk_ahci_disk.py
EXT2_DISK_TOOL := tools/scripts/mk_ext2_disk.py
FAT_DISK_TOOL := tools/scripts/mk_fat_disk.py
EXFAT_DISK_TOOL := tools/scripts/make_exfat_test_disk.py
ISO9660_DISK_TOOL := tools/scripts/make_iso9660_test_disks.py
MINIX_DISK_TOOL := tools/scripts/make_minix_test_disks.py

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
EXFAT_TEST_DISK := $(AHCI_TEST_DISK_DIR)/exfat.img
ISO9660_TEST_DISK := $(AHCI_TEST_DISK_DIR)/iso9660/iso9660-partitions.img
MINIX_TEST_DISK := $(AHCI_TEST_DISK_DIR)/minix/minix-partitions.img

NETBOOT_NAME ?= rhkernel
NETBOOT_HOST ?= 192.168.50.1
NETBOOT_HTTP_PORT ?= 8000
NETBOOT_IFACE ?= enp3s0
NETBOOT_PLATFORM ?= uefi
NETBOOT_DHCP_START ?= 192.168.50.20
NETBOOT_DHCP_END ?= 192.168.50.100
NETBOOT_ROOT ?= /srv/netboot
NETBOOT_HTTP_ROOT ?= $(NETBOOT_ROOT)/http
NETBOOT_TFTP_ROOT ?= $(NETBOOT_ROOT)/tftp
NETBOOT_GRUB_I386_PC_DIR ?= /usr/lib/grub/i386-pc
NETBOOT_URL := http://$(NETBOOT_HOST):$(NETBOOT_HTTP_PORT)/$(NETBOOT_NAME)
NETBOOT_BUILD_DIR := build/netboot
NETBOOT_HTTP_DIR := $(NETBOOT_HTTP_ROOT)/$(NETBOOT_NAME)
NETBOOT_GRUB_BIOS_HTTP_MODULE_DIR := $(NETBOOT_HTTP_DIR)/grub/i386-pc
NETBOOT_IPXE_SCRIPT := $(NETBOOT_BUILD_DIR)/boot.ipxe
NETBOOT_GRUB_CFG := $(NETBOOT_BUILD_DIR)/grub.cfg
NETBOOT_DNSMASQ_CONF := $(NETBOOT_BUILD_DIR)/dnsmasq-rhkernel.conf
NETBOOT_GRUB_EFI := $(NETBOOT_BUILD_DIR)/grubnetx64.efi
NETBOOT_GRUB_BIOS := $(NETBOOT_BUILD_DIR)/grub.pxe
NETBOOT_GRUB_BIOS_EMBED := $(NETBOOT_BUILD_DIR)/grub-bios-embed.cfg
NETBOOT_UNDIONLY := $(NETBOOT_TFTP_ROOT)/undionly.kpxe
NETBOOT_SNPONLY := $(NETBOOT_TFTP_ROOT)/snponly.efi
NETBOOT_EMBED_IPXE_SCRIPT := $(NETBOOT_BUILD_DIR)/embedded.ipxe
NETBOOT_IPXE_REPO ?= https://github.com/ipxe/ipxe.git
NETBOOT_IPXE_SRC ?= $(TOOL_CACHE_DIR)/ipxe
NETBOOT_IPXE_MAKEFILE := $(NETBOOT_IPXE_SRC)/src/Makefile
NETBOOT_GRUB_COMMON_MODULES := normal configfile echo halt reboot test net http multiboot2
NETBOOT_GRUB_EFI_MODULES := $(NETBOOT_GRUB_COMMON_MODULES) efinet
NETBOOT_GRUB_BIOS_MODULES := normal configfile echo halt reboot test net http pxe multiboot2 boot relocator

ifeq ($(NETBOOT_PLATFORM),uefi)
NETBOOT_BOOTLOADERS := $(NETBOOT_GRUB_EFI)
NETBOOT_CHAINLOADERS := $(NETBOOT_SNPONLY)
else ifeq ($(NETBOOT_PLATFORM),bios)
NETBOOT_BOOTLOADERS := $(NETBOOT_GRUB_BIOS)
NETBOOT_CHAINLOADERS := $(NETBOOT_UNDIONLY)
else ifeq ($(NETBOOT_PLATFORM),both)
NETBOOT_BOOTLOADERS := $(NETBOOT_GRUB_EFI) $(NETBOOT_GRUB_BIOS)
NETBOOT_CHAINLOADERS := $(NETBOOT_SNPONLY) $(NETBOOT_UNDIONLY)
else
$(error NETBOOT_PLATFORM must be uefi, bios, or both)
endif
FAT12_TEST_DISK_SIZE ?= 1440K
FAT16_TEST_DISK_SIZE ?= 16M
FAT32_TEST_DISK_SIZE ?= 64M
EXFAT_TEST_DISK_SIZE_MIB ?= 16
FAT_TEST_DISK_IMAGES := $(FAT12_TEST_DISK) $(FAT16_TEST_DISK) $(FAT32_TEST_DISK)

USB_STORAGE_DISK_SIZE ?= 16M
USB_STORAGE_DISK := $(AHCI_TEST_DISK_DIR)/usb-storage-fat16.img
USB_TEST_HCD ?= ehci
VIRTIO_MODERN_EXT2_DISK := $(AHCI_TEST_DISK_DIR)/virtio-modern-ext2.img
VIRTIO_LEGACY_EXT2_DISK := $(AHCI_TEST_DISK_DIR)/virtio-legacy-ext2.img
VIRTIO_EXT2_DISK_SIZE ?= 32M
AHCI_TEST_DISK_IMAGES := $(AHCI_MBR_DISK) $(AHCI_GPT_DISK) $(AHCI_EXT2_DISK) $(FAT_TEST_DISK_IMAGES) $(EXFAT_TEST_DISK) $(ISO9660_TEST_DISK) $(MINIX_TEST_DISK) $(VIRTIO_MODERN_EXT2_DISK) $(VIRTIO_LEGACY_EXT2_DISK)

ifeq ($(AHCI_TEST),1)
QEMU_AHCI_FLAGS := -device ich9-ahci,id=ahci0,bus=pcie.0,addr=0x9 \
    -drive if=none,id=bootiso,file=dist/x86_64/kernel.iso,media=cdrom,readonly=on \
    -device ide-cd,drive=bootiso,bus=ahci0.5,bootindex=0 \
    -drive if=none,id=ahci_mbr,file=$(AHCI_MBR_DISK),format=raw \
    -device ide-hd,drive=ahci_mbr,bus=ahci0.0 \
    -drive if=none,id=ahci_gpt,file=$(AHCI_GPT_DISK),format=raw \
    -device ide-hd,drive=ahci_gpt,bus=ahci0.1 \
    -drive if=none,id=ahci_ext2,file=$(AHCI_EXT2_DISK),format=raw \
    -device ide-hd,drive=ahci_ext2,bus=ahci0.2 \
    -drive if=none,id=iso9660,file=$(ISO9660_TEST_DISK),format=raw \
    -device ide-hd,drive=iso9660,bus=ahci0.3 \
    -drive if=none,id=minix,file=$(MINIX_TEST_DISK),format=raw \
    -device ide-hd,drive=minix,bus=ahci0.4 \
    -device ich9-ahci,id=ahci1,bus=pcie.0,addr=0xa \
    -drive if=none,id=fat12,file=$(FAT12_TEST_DISK),format=raw \
    -device ide-hd,drive=fat12,bus=ahci1.0 \
    -drive if=none,id=fat16,file=$(FAT16_TEST_DISK),format=raw \
    -device ide-hd,drive=fat16,bus=ahci1.1 \
    -drive if=none,id=fat32,file=$(FAT32_TEST_DISK),format=raw \
    -device ide-hd,drive=fat32,bus=ahci1.2 \
    -drive if=none,id=exfat,file=$(EXFAT_TEST_DISK),format=raw \
    -device ide-hd,drive=exfat,bus=ahci1.3 \
    -drive if=none,id=virtio_modern_ext2,file=$(VIRTIO_MODERN_EXT2_DISK),format=raw \
    -device virtio-blk-pci,drive=virtio_modern_ext2,bus=pcie.0,addr=0xb,disable-legacy=on,disable-modern=off \
    -drive if=none,id=virtio_legacy_ext2,file=$(VIRTIO_LEGACY_EXT2_DISK),format=raw \
    -device virtio-blk-pci,drive=virtio_legacy_ext2,bus=bridge1,addr=0x2,disable-modern=on,disable-legacy=off

QEMU_AHCI_DEPS := $(AHCI_TEST_DISK_IMAGES)
QEMU_BOOT_FLAGS := -boot d
else
QEMU_AHCI_FLAGS := -device ich9-ahci,id=ahci0,bus=pcie.0,addr=0x9 \
    -drive if=none,id=bootiso,file=dist/x86_64/kernel.iso,media=cdrom,readonly=on \
    -device ide-cd,drive=bootiso,bus=ahci0.5,bootindex=0

QEMU_AHCI_DEPS :=
QEMU_BOOT_FLAGS := -boot d
endif

QEMU_USB_DEPS := $(USB_STORAGE_DISK)
ifeq ($(USB_TEST_HCD),uhci)
QEMU_USB_FLAGS := -device usb-kbd,bus=uhci0.0 \
    -drive if=none,id=usb_msc,file=$(USB_STORAGE_DISK),format=raw \
    -device usb-storage,bus=uhci0.0,drive=usb_msc,removable=on,serial=RHUSBTEST001
else ifeq ($(USB_TEST_HCD),ehci)
QEMU_USB_FLAGS := -device usb-kbd,bus=uhci0.0 \
    -device ich9-usb-ehci1,id=ehci0,bus=pcie.0,addr=0xc \
    -drive if=none,id=usb_msc,file=$(USB_STORAGE_DISK),format=raw \
    -device usb-storage,bus=ehci0.0,drive=usb_msc,removable=on,serial=RHEHCITEST001
else
$(error USB_TEST_HCD must be uhci or ehci)
endif

# -d int,cpu_reset,guest_errors,in_asm,exec
QEMUFLAGS := -nodefaults \
            -d int,cpu_reset,guest_errors,in_asm,exec \
            -no-reboot -no-shutdown \
            -machine q35,sata=off \
            $(QEMU_ACCEL_FLAGS) \
            -device VGA \
            -D qemu.log \
            -m $(QEMU_MEM) \
            -smp $(QEMU_SMP) \
            -netdev user,id=net0 \
            -netdev user,id=net1 \
            -device pcie-pci-bridge,id=bridge1,bus=pcie.0,addr=0x6 \
            -device piix3-usb-uhci,id=uhci0,bus=bridge1,addr=0x3 \
            $(QEMU_USB_FLAGS) \
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
USER_APP_PUBLIC_HEADERS := $(USERSPACE_PUBLIC_HEADERS)
USER_APP_MAKEFILES := $(USER_APP_MAKEFILE_FILES) $(TOP_DIR)/src/user/common.mk $(TOP_DIR)/src/user/linker.ld $(TOP_DIR)/base.mk
USER_APPS_STAMP := build/.user-apps.stamp

MODULE_DIRS := $(wildcard $(TOP_DIR)/src/module/*/)
MODULES := $(filter-out $(TOP_DIR)/src/module/common.mk, $(MODULE_DIRS))
MODULE_SOURCE_FILES := $(shell find $(TOP_DIR)/src/module \( -path '*/src/*' -o -path '*/include/*' \) \( -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.hpp' \) 2>/dev/null)
MODULE_PUBLIC_HEADERS := $(shell find $(TOP_DIR)/src/public \( -name '*.h' -o -name '*.hpp' \) 2>/dev/null)
MODULE_MAKEFILES := $(foreach dir,$(MODULES),$(dir)Makefile) $(TOP_DIR)/src/module/common.mk $(TOP_DIR)/base.mk
MODULES_STAMP := build/.modules.stamp

.PHONY: FORCE libc libcppabi
FORCE:

libc: $(SYSROOT)/lib/libc.a
libcppabi: $(SYSROOT)/lib/libcppabi.a

$(LIBC_STAMP): $(LIBC_OBJ_FILES) $(LIBC_HEADERS) $(USERSPACE_PUBLIC_HEADERS) | $(TOOLCHAIN_STAMP)
	$(Q)rm -rf $(SYSROOT)
	@mkdir -p $(SYSROOT)/lib $(SYSROOT)/include
	@printf "$(YELLOW)[LIBC]$(NC) Installing headers...\n"
	$(Q)cp -a $(LIBC_SRC_DIR)/include/. $(SYSROOT)/include/
	$(Q)cp -a $(USERSPACE_PUBLIC_HEADERS) $(SYSROOT)/include/
	@printf "$(YELLOW)[LIBC]$(NC) Archiving libc.a...\n"
	$(Q)$(AR) rcs $(SYSROOT)/lib/libc.a $(LIBC_LIB_OBJ_FILES)
	$(Q)$(RANLIB) $(SYSROOT)/lib/libc.a
	@touch $@

$(SYSROOT)/lib/libc.a: $(LIBC_STAMP)
	$(Q)test -r $@
	$(Q)$(AR) t $@ >/dev/null

$(LIBCPPABI_STAMP): $(LIBCPPABI_OBJ_FILES) $(LIBC_STAMP) | $(TOOLCHAIN_STAMP)
	@mkdir -p $(SYSROOT)/lib
	@printf "$(YELLOW)[CXXABI]$(NC) Archiving libcppabi.a...\n"
	$(Q)$(AR) rcs $(SYSROOT)/lib/libcppabi.a $(LIBCPPABI_OBJ_FILES)
	$(Q)$(RANLIB) $(SYSROOT)/lib/libcppabi.a
	@touch $@

$(SYSROOT)/lib/libcppabi.a: $(LIBCPPABI_STAMP)
	$(Q)test -r $@
	$(Q)$(AR) t $@ >/dev/null

.PHONY: build-user-apps $(USER_APPS)
build-user-apps: $(USER_APPS)

$(USER_APPS): libc libcppabi $(LIBC_CRT0) | $(TOOLCHAIN_STAMP)
	@printf "$(YELLOW)[BUILD]$(NC) User App: $(notdir $(patsubst %/,%,$@))\n"
	+$(Q)$(MAKE) -C $@ all --no-print-directory -s

$(USER_APPS_STAMP): $(USER_APP_SOURCE_FILES) $(USER_APP_PUBLIC_HEADERS) $(USER_APP_MAKEFILES) $(LIBC_STAMP) $(LIBCPPABI_STAMP) $(LIBC_CRT0) | $(TOOLCHAIN_STAMP)
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
	$(Q)mkdir -p $(INITRAMFS_STAGE_DIR)/bin $(INITRAMFS_STAGE_DIR)/lib/modules $(INITRAMFS_STAGE_DIR)/tmp $(INITRAMFS_STAGE_DIR)/mnt
	$(Q)if test -d $(INITRAMFS_SRC); then cp -a $(INITRAMFS_SRC)/. $(INITRAMFS_STAGE_DIR)/; fi
	$(Q)rm -rf $(INITRAMFS_STAGE_DIR)/bin $(INITRAMFS_STAGE_DIR)/lib/modules
	$(Q)mkdir -p $(INITRAMFS_STAGE_DIR)/bin $(INITRAMFS_STAGE_DIR)/lib/modules $(INITRAMFS_STAGE_DIR)/tmp $(INITRAMFS_STAGE_DIR)/mnt
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

build/x86_64/security/stack_protector.cpp.o: src/impl/x86_64/security/stack_protector.cpp Makefile base.mk $(KERNEL_BUILD_CFG) | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	@printf "$(GREEN)[CXX]$(NC) $<\n"
	$(Q)$(CXX) $(DEPFLAGS) -c $(filter-out -flto -fstack-protector-strong -mstack-protector-guard=global,$(CXXFLAGS)) -fno-lto -fno-stack-protector $< -o $@

build/x86_64/%.c.o: src/impl/x86_64/%.c Makefile base.mk $(KERNEL_BUILD_CFG) | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	@printf "$(GREEN)[CC]$(NC) $<\n"
	$(Q)$(CC) $(DEPFLAGS) -c $(CFLAGS) $< -o $@

build/x86_64/%.cpp.o: src/impl/x86_64/%.cpp Makefile base.mk $(KERNEL_BUILD_CFG) | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	@printf "$(GREEN)[CXX]$(NC) $<\n"
	$(Q)$(CXX) $(DEPFLAGS) -c $(CXXFLAGS) $< -o $@

.PHONY: build-x86_64
build-x86_64: libc $(KERNEL_ISO)

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


$(NETBOOT_IPXE_SCRIPT): targets/x86_64/netboot/boot.ipxe.in Makefile
	@mkdir -p $(dir $@)
	$(Q)sed \
		-e 's|@NETBOOT_URL@|$(NETBOOT_URL)|g' \
		$< > $@.tmp
	$(Q)mv $@.tmp $@

$(NETBOOT_EMBED_IPXE_SCRIPT): targets/x86_64/netboot/embedded.ipxe.in Makefile
	@mkdir -p $(dir $@)
	$(Q)sed \
		-e 's|@NETBOOT_URL@|$(NETBOOT_URL)|g' \
		$< > $@.tmp
	$(Q)mv $@.tmp $@

$(NETBOOT_GRUB_CFG): targets/x86_64/netboot/grub.cfg.in Makefile
	@mkdir -p $(dir $@)
	$(Q)sed \
		-e 's|@NETBOOT_HOST@|$(NETBOOT_HOST)|g' \
		-e 's|@NETBOOT_HTTP_PORT@|$(NETBOOT_HTTP_PORT)|g' \
		-e 's|@NETBOOT_NAME@|$(NETBOOT_NAME)|g' \
		$< > $@.tmp
	$(Q)mv $@.tmp $@

$(NETBOOT_DNSMASQ_CONF): targets/x86_64/netboot/dnsmasq-rhkernel.conf.in Makefile
	@mkdir -p $(dir $@)
	$(Q)sed \
		-e 's|@NETBOOT_IFACE@|$(NETBOOT_IFACE)|g' \
		-e 's|@NETBOOT_DHCP_START@|$(NETBOOT_DHCP_START)|g' \
		-e 's|@NETBOOT_DHCP_END@|$(NETBOOT_DHCP_END)|g' \
		-e 's|@NETBOOT_TFTP_ROOT@|$(NETBOOT_TFTP_ROOT)|g' \
		-e 's|@NETBOOT_HOST@|$(NETBOOT_HOST)|g' \
		-e 's|@NETBOOT_URL@|$(NETBOOT_URL)|g' \
		$< > $@.tmp
	$(Q)mv $@.tmp $@

$(NETBOOT_GRUB_EFI): $(NETBOOT_GRUB_CFG)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[NETBOOT]$(NC) Building UEFI GRUB net image\n"
	$(Q)grub-mkstandalone -O x86_64-efi -o $@ \
		--modules="$(NETBOOT_GRUB_EFI_MODULES)" \
		"boot/grub/grub.cfg=$(NETBOOT_GRUB_CFG)"

$(NETBOOT_GRUB_BIOS_EMBED): targets/x86_64/netboot/grub-bios-embed.cfg.in Makefile
	@mkdir -p $(dir $@)
	$(Q)sed \
		-e 's|@NETBOOT_HOST@|$(NETBOOT_HOST)|g' \
		-e 's|@NETBOOT_HTTP_PORT@|$(NETBOOT_HTTP_PORT)|g' \
		-e 's|@NETBOOT_NAME@|$(NETBOOT_NAME)|g' \
		$< > $@.tmp
	$(Q)mv $@.tmp $@

$(NETBOOT_GRUB_BIOS): $(NETBOOT_GRUB_CFG) $(NETBOOT_GRUB_BIOS_EMBED)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[NETBOOT]$(NC) Building minimal BIOS GRUB PXE image\n"
	$(Q)grub-mkimage -O i386-pc-pxe -o $@ \
		-p '(pxe)' -c $(NETBOOT_GRUB_BIOS_EMBED) \
		$(NETBOOT_GRUB_BIOS_MODULES)

$(NETBOOT_IPXE_MAKEFILE):
	@mkdir -p $(dir $(NETBOOT_IPXE_SRC))
	@if [ -d "$(NETBOOT_IPXE_SRC)/.git" ]; then \
		printf "$(YELLOW)[NETBOOT]$(NC) Updating cached iPXE source\n"; \
		git -C "$(NETBOOT_IPXE_SRC)" fetch --depth=1 origin master; \
		git -C "$(NETBOOT_IPXE_SRC)" reset --hard FETCH_HEAD; \
	else \
		printf "$(YELLOW)[NETBOOT]$(NC) Cloning iPXE source into $(NETBOOT_IPXE_SRC)\n"; \
		rm -rf "$(NETBOOT_IPXE_SRC)"; \
		git clone --depth=1 "$(NETBOOT_IPXE_REPO)" "$(NETBOOT_IPXE_SRC)"; \
	fi

$(NETBOOT_UNDIONLY): $(NETBOOT_EMBED_IPXE_SCRIPT) $(NETBOOT_IPXE_MAKEFILE)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[NETBOOT]$(NC) Building BIOS iPXE with embedded RHKernel script\n"
	$(Q)$(MAKE) -C $(NETBOOT_IPXE_SRC)/src bin/undionly.kpxe EMBED=$(abspath $(NETBOOT_EMBED_IPXE_SCRIPT))
	$(Q)cp $(NETBOOT_IPXE_SRC)/src/bin/undionly.kpxe $@.tmp
	$(Q)mv $@.tmp $@

$(NETBOOT_SNPONLY): $(NETBOOT_EMBED_IPXE_SCRIPT) $(NETBOOT_IPXE_MAKEFILE)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[NETBOOT]$(NC) Building UEFI iPXE with embedded RHKernel script\n"
	$(Q)$(MAKE) -C $(NETBOOT_IPXE_SRC)/src bin-x86_64-efi/snponly.efi EMBED=$(abspath $(NETBOOT_EMBED_IPXE_SCRIPT))
	$(Q)cp $(NETBOOT_IPXE_SRC)/src/bin-x86_64-efi/snponly.efi $@.tmp
	$(Q)mv $@.tmp $@

.PHONY: netboot-install netboot-http netboot-print
netboot-install: $(KERNEL_BIN) $(INITRAMFS_BIN) $(NETBOOT_IPXE_SCRIPT) $(NETBOOT_BOOTLOADERS) $(NETBOOT_DNSMASQ_CONF) $(NETBOOT_CHAINLOADERS)
	@mkdir -p $(NETBOOT_HTTP_DIR)/boot $(NETBOOT_HTTP_DIR)/grub $(NETBOOT_TFTP_ROOT)
	@printf "$(YELLOW)[NETBOOT]$(NC) Installing HTTP artifacts into $(NETBOOT_HTTP_DIR)\n"
	$(Q)cp $(KERNEL_BIN) $(NETBOOT_HTTP_DIR)/boot/kernel.bin
	$(Q)cp $(INITRAMFS_BIN) $(NETBOOT_HTTP_DIR)/boot/initramfs.cpio
	$(Q)cp $(NETBOOT_IPXE_SCRIPT) $(NETBOOT_HTTP_DIR)/boot.ipxe
	$(Q)cp $(NETBOOT_GRUB_CFG) $(NETBOOT_HTTP_DIR)/grub/grub.cfg
	$(Q)if [ "$(NETBOOT_PLATFORM)" != "uefi" ]; then \
		test -d $(NETBOOT_GRUB_I386_PC_DIR); \
		rm -rf $(NETBOOT_GRUB_BIOS_HTTP_MODULE_DIR); \
		mkdir -p $(NETBOOT_GRUB_BIOS_HTTP_MODULE_DIR); \
		cp -a $(NETBOOT_GRUB_I386_PC_DIR)/. $(NETBOOT_GRUB_BIOS_HTTP_MODULE_DIR)/; \
	fi
	$(Q)if [ -f $(NETBOOT_GRUB_EFI) ]; then cp $(NETBOOT_GRUB_EFI) $(NETBOOT_HTTP_DIR)/grubnetx64.efi; fi
	$(Q)if [ -f $(NETBOOT_GRUB_BIOS) ]; then cp $(NETBOOT_GRUB_BIOS) $(NETBOOT_HTTP_DIR)/grub.pxe; fi
	@printf "$(YELLOW)[NETBOOT]$(NC) dnsmasq config generated at $(NETBOOT_DNSMASQ_CONF)\n"

netboot-http: netboot-install
	@printf "$(GREEN)[NETBOOT]$(NC) Serving $(NETBOOT_HTTP_ROOT) on $(NETBOOT_HOST):$(NETBOOT_HTTP_PORT)\n"
	$(Q)cd $(NETBOOT_HTTP_ROOT) && $(PYTHON) -m http.server $(NETBOOT_HTTP_PORT) --bind $(NETBOOT_HOST)

netboot-print: $(NETBOOT_DNSMASQ_CONF)
	@printf 'HTTP root:     %s\n' '$(NETBOOT_HTTP_ROOT)'
	@printf 'TFTP root:     %s\n' '$(NETBOOT_TFTP_ROOT)'
	@printf 'Boot script:   %s/boot.ipxe\n' '$(NETBOOT_URL)'
	@printf 'Embedded iPXE: %s\n' '$(NETBOOT_EMBED_IPXE_SCRIPT)'
	@printf 'BIOS GRUB cfg: %s/grub/grub.cfg\n' '$(NETBOOT_URL)'
	@printf 'dnsmasq conf:  %s\n' '$(NETBOOT_DNSMASQ_CONF)'
	@printf 'Interface IP:  sudo ip addr add %s/24 dev %s || true\n' '$(NETBOOT_HOST)' '$(NETBOOT_IFACE)'
	@printf 'Install conf:  sudo cp %s /etc/dnsmasq.d/rhkernel-pxe.conf\n' '$(NETBOOT_DNSMASQ_CONF)'

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

$(USB_STORAGE_DISK): Makefile $(FAT_DISK_TOOL)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[DISK]$(NC) Creating FAT16 USB storage disk $@ ($(USB_STORAGE_DISK_SIZE))\n"
	$(Q)$(PYTHON) $(FAT_DISK_TOOL) fat16 $@ $(USB_STORAGE_DISK_SIZE)

$(FAT32_TEST_DISK): Makefile $(FAT_DISK_TOOL)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[DISK]$(NC) Creating FAT32 test disk $@ ($(FAT32_TEST_DISK_SIZE))\n"
	$(Q)$(PYTHON) $(FAT_DISK_TOOL) fat32 $@ $(FAT32_TEST_DISK_SIZE)

$(EXFAT_TEST_DISK): Makefile $(EXFAT_DISK_TOOL)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[DISK]$(NC) Creating exFAT test disk $@ ($(EXFAT_TEST_DISK_SIZE_MIB) MiB)\n"
	$(Q)$(PYTHON) $(EXFAT_DISK_TOOL) $@ --size-mib $(EXFAT_TEST_DISK_SIZE_MIB)

$(VIRTIO_MODERN_EXT2_DISK): Makefile $(EXT2_DISK_TOOL)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[DISK]$(NC) Creating modern VirtIO ext2 test disk $@ ($(VIRTIO_EXT2_DISK_SIZE))\n"
	$(Q)$(PYTHON) $(EXT2_DISK_TOOL) $@ $(VIRTIO_EXT2_DISK_SIZE)

$(VIRTIO_LEGACY_EXT2_DISK): Makefile $(EXT2_DISK_TOOL)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[DISK]$(NC) Creating legacy VirtIO ext2 test disk $@ ($(VIRTIO_EXT2_DISK_SIZE))\n"
	$(Q)$(PYTHON) $(EXT2_DISK_TOOL) $@ $(VIRTIO_EXT2_DISK_SIZE)

$(ISO9660_TEST_DISK): Makefile $(ISO9660_DISK_TOOL)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[DISK]$(NC) Creating ISO9660 partition test disk $@\n"
	$(Q)$(PYTHON) $(ISO9660_DISK_TOOL) $(dir $@)

$(MINIX_TEST_DISK): Makefile $(MINIX_DISK_TOOL)
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[DISK]$(NC) Creating Minix partition test disk $@\n"
	$(Q)$(PYTHON) $(MINIX_DISK_TOOL) $(dir $@)

clean-ahci-test-disks:
	@printf "$(RED)[CLEAN]$(NC) Removing AHCI test disks...\n"
	$(Q)rm -rf $(AHCI_TEST_DISK_DIR)


print-qemu: build-x86_64 $(QEMU_AHCI_DEPS) $(QEMU_USB_DEPS)
	@printf '%s\n' 'qemu-system-x86_64 $(QEMUFLAGS)'

.PHONY: run
run: build-x86_64 $(QEMU_AHCI_DEPS) $(QEMU_USB_DEPS)
	@printf "$(GREEN)--- Starting QEMU ---$(NC)\n"
	$(Q)qemu-system-x86_64 $(QEMUFLAGS)

.PHONY: run-ehci print-qemu-ehci debug-ehci
run-ehci:
	$(Q)$(MAKE) run USB_TEST_HCD=ehci --no-print-directory

print-qemu-ehci:
	$(Q)$(MAKE) print-qemu USB_TEST_HCD=ehci --no-print-directory

debug-ehci:
	$(Q)$(MAKE) debug USB_TEST_HCD=ehci --no-print-directory


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
