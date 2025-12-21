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

TOOLS_DIR := tools
TOOLS_ZIP := $(TOOLS_DIR)/x86_64-elf-tools-linux.zip
TOOLS_STAMP := $(TOOLS_DIR)/.installed
TOOLS_URL := https://github.com/lordmilko/i686-elf-tools/releases/download/15.2.0/x86_64-elf-tools-linux.zip

CC  := $(TOOLS_DIR)/bin/x86_64-elf-gcc
CXX := $(TOOLS_DIR)/bin/x86_64-elf-g++
LD  := $(TOOLS_DIR)/bin/x86_64-elf-ld

FONT_TTF    := src/assets/font.ttf
FONT_BIN    := build/assets/font.bin
FONT_SCRIPT := tools/scripts/gen_font.py

VENV           := .venv
VENV_PYTHON    := $(VENV)/bin/python3
VENV_STAMP     := $(VENV)/.installed

QEMUFLAGS := -d int,cpu_reset \
			-D qemu.log \
			-cpu qemu64,+pdpe1gb \
			-cdrom dist/x86_64/kernel.iso \
			-serial stdio

COMMON_CFLAGS := \
	-ffreestanding \
	-fno-unwind-tables \
	-fno-asynchronous-unwind-tables \
	-fno-stack-protector \
	-fno-pic \
	-mcmodel=kernel \
	-mno-red-zone \
	-m64 \
	-Wall -Wextra \
	-I src/intf

NASMFLAGS := -f elf64 -w-zeroing

ifeq ($(DEBUG),1)
	CFLAGS  := $(COMMON_CFLAGS) -O0 -g -DDEBUG
	LDFLAGS :=
	NASMFLAGS := $(NASMFLAGS) -g -F dwarf
else
	CFLAGS  := $(COMMON_CFLAGS) -O2
	LDFLAGS := -s
endif

CXXFLAGS := $(CFLAGS) -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit

# --- Rules ---

$(TOOLS_STAMP):
	mkdir -p $(TOOLS_DIR)
	curl -L $(TOOLS_URL) -o $(TOOLS_ZIP)
	unzip -o $(TOOLS_ZIP) -d $(TOOLS_DIR)
	touch $@

.PHONY: clean-tools
clean-tools:
	rm -rf tools

$(VENV_STAMP):
	test -d $(VENV) || python3 -m venv $(VENV)
	$(VENV_PYTHON) -m pip install Pillow
	touch $@

.PHONY: clean-venv
clean-tools:
	rm -rf $(VENV)

$(FONT_BIN): $(FONT_TTF) $(FONT_SCRIPT) | $(VENV_STAMP)
	@mkdir -p $(dir $@)
	$(VENV_PYTHON) $(FONT_SCRIPT) $(FONT_TTF) $@

build/x86_64/%.asm.o: src/impl/x86_64/%.asm | $(FONT_BIN)
	mkdir -p $(dir $@) && \
	nasm $(NASMFLAGS) $< -o $@

build/x86_64/%.c.o: src/impl/x86_64/%.c | $(TOOLS_STAMP)
	mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@

build/x86_64/%.cpp.o: src/impl/x86_64/%.cpp | $(TOOLS_STAMP)
	mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) $< -o $@

.PHONY: build-x86_64
build-x86_64: $(TOOLS_STAMP) $(x86_64_object_files)
	mkdir -p dist/x86_64
	$(LD) -n $(LFDLAGS) -o dist/x86_64/kernel.bin -T targets/x86_64/linker.ld $(x86_64_object_files)
	mkdir -p targets/x86_64/iso/boot
	cp dist/x86_64/kernel.bin targets/x86_64/iso/boot/kernel.bin
	grub-mkrescue /usr/lib/grub/i386-pc -o dist/x86_64/kernel.iso targets/x86_64/iso

.PHONY: run
run: build-x86_64
	qemu-system-x86_64 $(QEMUFLAGS)

.PHONY: debug
debug: build-x86_64
	@set -e; set -x; \
	qemu-system-x86_64 \
		$(QEMUFLAGS) \
		-S -s -no-shutdown -no-reboot & \
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

.PHONY: clean-all
clean-all: clean clean-venv clean-tools
	

.PHONY: doc
doc:
	doxygen Doxyfile