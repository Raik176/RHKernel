# Assembly
x86_64_asm_source_files := $(shell find src/impl/x86_64 -name '*.asm')
x86_64_asm_object_files := $(patsubst src/impl/x86_64/%.asm, build/x86_64/%.asm.o, $(x86_64_asm_source_files))

# C++
x86_64_cpp_source_files := $(shell find src/impl/x86_64 -name '*.cpp')
x86_64_cpp_object_files := $(patsubst src/impl/x86_64/%.cpp, build/x86_64/%.cpp.o, $(x86_64_cpp_source_files))

# C
x86_64_c_source_files := $(shell find src/impl/x86_64 -name '*.c')
x86_64_c_object_files := $(patsubst src/impl/x86_64/%.c, build/x86_64/%.c.o, $(x86_64_c_source_files))

# All object files
x86_64_object_files := $(x86_64_asm_object_files) $(x86_64_c_object_files) $(x86_64_cpp_object_files)

# --- Configuration ---
# Default to Release build (DEBUG=0). To debug, run: make DEBUG=1
DEBUG ?= 0

COMMON_CFLAGS := \
	-ffreestanding \
	-fno-unwind-tables \
	-fno-asynchronous-unwind-tables \
	-mcmodel=kernel \
	-mno-red-zone \
	-m64 \
	-Wall -Wextra \
	-I src/intf

ifeq ($(DEBUG),1)
	CFLAGS  := $(COMMON_CFLAGS) -O0 -g
	LDFLAGS :=
	NASMFLAGS := -f elf64 -g -F dwarf
else
	CFLAGS  := $(COMMON_CFLAGS) -O2
	LDFLAGS := -s
	NASMFLAGS := -f elf64
endif

CXXFLAGS := $(CFLAGS) -fno-exceptions -fno-rtti

# --- Rules ---

build/x86_64/%.asm.o: src/impl/x86_64/%.asm
	mkdir -p $(dir $@) && \
	nasm $(NASMFLAGS) $< -o $@

build/x86_64/%.c.o: src/impl/x86_64/%.c
	mkdir -p $(dir $@)
	x86_64-elf-gcc -c $(CFLAGS) $< -o $@

build/x86_64/%.cpp.o: src/impl/x86_64/%.cpp
	mkdir -p $(dir $@) && \
	x86_64-elf-g++ -c $(CXXFLAGS) $< -o $@

.PHONY: build-x86_64
build-x86_64: $(x86_64_object_files)
	mkdir -p dist/x86_64
	x86_64-elf-ld -n $(ld_flags) -o dist/x86_64/kernel.bin -T targets/x86_64/linker.ld $(x86_64_object_files)
	mkdir -p targets/x86_64/iso/boot
	cp dist/x86_64/kernel.bin targets/x86_64/iso/boot/kernel.bin
	grub-mkrescue /usr/lib/grub/i386-pc -o dist/x86_64/kernel.iso targets/x86_64/iso

# --- Run & Debug Targets ---

.PHONY: run
run: build-x86_64
	qemu-system-x86_64 -d int,cpu_reset -D qemu.log -cdrom dist/x86_64/kernel.iso

.PHONY: debug
debug: build-x86_64
	@set -e; set -x; \
	qemu-system-x86_64 \
		-cdrom dist/x86_64/kernel.iso \
		-S -s -d int -no-shutdown -no-reboot & \
	QEMU_PID=$$!; \
	\
	gdb dist/x86_64/kernel.bin \
		-ex 'target remote localhost:1234' \
		-ex 'set disassembly-flavor intel' \
	\
	stty sane; \
	if ps -p $$QEMU_PID > /dev/null; then \
		kill -9 $$QEMU_PID; \
	fi

.PHONY: clean
clean:
	rm -rf build dist