TOP_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

.DELETE_ON_ERROR:
.SUFFIXES:


ifneq ($(shell tput colors 2>/dev/null | awk '$$1 >= 8 { print "yes" }'),)
    GREEN  := \033[1;32m
    BLUE   := \033[1;34m
    YELLOW := \033[1;33m
    RED    := \033[1;31m
    NC     := \033[0m
else
    GREEN  :=
    BLUE   :=
    YELLOW :=
    RED    :=
    NC     :=
endif

SYSROOT := $(TOP_DIR)/build/sysroot/x86_64-elf

CURL ?= curl
CURL_FLAGS ?= -fL --connect-timeout 20 --retry 5 --retry-delay 2 --retry-connrefused

GLOBAL_CFLAGS  := -m64 -Wall -Wextra -flto -ffunction-sections -fdata-sections
GLOBAL_LDFLAGS := --gc-sections

KERNEL_CODEGEN_CFLAGS := -mno-red-zone -mgeneral-regs-only -mno-mmx -mno-sse -mno-sse2

USER_PIE_CFLAGS := -fPIE
USER_OPT_CFLAGS ?= -O2 -fomit-frame-pointer -fno-plt -fno-semantic-interposition
USER_RUNTIME_CFLAGS := $(USER_PIE_CFLAGS) $(USER_OPT_CFLAGS)

DEPFLAGS = -MMD -MP -MF $(@:.o=.d) -MT $@
NASMDEPFLAGS = -MD $(@:.o=.d) -MT $@


ifeq ($(V),1)
    Q :=
else
    Q := @
endif

TOOLS_DIR := $(TOP_DIR)/tools
TOOL_CACHE_DIR ?= $(TOP_DIR)/.cache/tools
TOOLCHAIN_DIR := $(TOOL_CACHE_DIR)/x86_64-elf-toolchain
TOOLCHAIN_ZIP := $(TOOLCHAIN_DIR)/tools.zip
TOOLCHAIN_ZIP_TMP := $(TOOLCHAIN_ZIP).tmp
TOOLCHAIN_URL := https://github.com/lordmilko/i686-elf-tools/releases/download/15.2.0/x86_64-elf-tools-linux.zip
TOOLCHAIN_STAMP := $(TOOLCHAIN_DIR)/.installed
TOOLCHAIN_LOCK := $(TOOLCHAIN_DIR)/.install.lock

VENV ?= $(TOOL_CACHE_DIR)/venv
VENV_PYTHON := $(VENV)/bin/python3
VENV_PIP := $(VENV)/bin/pip
VENV_STAMP := $(VENV)/.installed
REQUIREMENTS := $(TOOLS_DIR)/requirements.txt

LIBC_SRC_DIR := $(TOP_DIR)/src/libc
LIBC_BUILD := $(TOP_DIR)/build/libc
LIBC_SRC_FILES := $(shell find $(LIBC_SRC_DIR)/src -name '*.c' 2>/dev/null)
LIBC_OBJ_FILES := $(patsubst $(LIBC_SRC_DIR)/src/%.c,$(LIBC_BUILD)/%.o,$(LIBC_SRC_FILES))
LIBC_CRT0 := $(LIBC_BUILD)/crt0.o
LIBC_LIB_OBJ_FILES := $(filter-out $(LIBC_CRT0),$(LIBC_OBJ_FILES))
LIBC_HEADERS := $(shell find $(LIBC_SRC_DIR)/include -name '*.h' 2>/dev/null)
USERSPACE_PUBLIC_HEADERS := $(TOP_DIR)/src/public/display.h $(TOP_DIR)/src/public/input.h $(TOP_DIR)/src/public/event.h $(TOP_DIR)/src/public/usb.h
LIBC_STAMP := $(LIBC_BUILD)/.libc_done

LIBCPPABI_SRC_DIR := $(TOP_DIR)/src/libcppabi
LIBCPPABI_BUILD := $(TOP_DIR)/build/libcppabi
LIBCPPABI_SRC_FILES := $(shell find $(LIBCPPABI_SRC_DIR)/src -name '*.cpp' 2>/dev/null)
LIBCPPABI_OBJ_FILES := $(patsubst $(LIBCPPABI_SRC_DIR)/src/%.cpp,$(LIBCPPABI_BUILD)/%.o,$(LIBCPPABI_SRC_FILES))
LIBCPPABI_STAMP := $(LIBCPPABI_BUILD)/.libcppabi_done

$(LIBCPPABI_BUILD)/%.o: $(LIBCPPABI_SRC_DIR)/src/%.cpp $(LIBC_HEADERS) $(TOP_DIR)/base.mk | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	$(Q)$(CXX) -ffreestanding -nostdinc -isystem $(LIBC_SRC_DIR)/include \
		$(DEPFLAGS) $(USER_RUNTIME_CFLAGS) -fno-exceptions -fno-rtti -nostdlib -c $< -o $@

$(LIBC_BUILD)/%.o: $(LIBC_SRC_DIR)/src/%.c $(LIBC_HEADERS) $(TOP_DIR)/base.mk | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	$(Q)$(CC) -ffreestanding -nostdinc -isystem $(LIBC_SRC_DIR)/include \
		$(DEPFLAGS) $(USER_RUNTIME_CFLAGS) -nostdlib -c $< -o $@

CC := $(TOOLCHAIN_DIR)/bin/x86_64-elf-gcc
CXX := $(TOOLCHAIN_DIR)/bin/x86_64-elf-g++
LD := $(TOOLCHAIN_DIR)/bin/x86_64-elf-ld
AS := $(TOOLCHAIN_DIR)/bin/x86_64-elf-as
AR := $(TOOLCHAIN_DIR)/bin/x86_64-elf-ar
RANLIB := $(TOOLCHAIN_DIR)/bin/x86_64-elf-ranlib
NM := $(TOOLCHAIN_DIR)/bin/x86_64-elf-nm
OBJCOPY := $(TOOLCHAIN_DIR)/bin/x86_64-elf-objcopy
OBJDUMP := $(TOOLCHAIN_DIR)/bin/x86_64-elf-objdump
READELF := $(TOOLCHAIN_DIR)/bin/x86_64-elf-readelf
STRIP := $(TOOLCHAIN_DIR)/bin/x86_64-elf-strip

MODULE_BINARIES_DIR := $(TOP_DIR)/build/bin/modules
USER_BINARIES_DIR := $(TOP_DIR)/build/bin/user

SUBMODULE_STAMP := build/.submodules_updated

define INSTALL_TOOLCHAIN
	if [ -x "$(CC)" ] && [ -x "$(CXX)" ] && [ -x "$(LD)" ]; then \
		touch "$@"; \
		exit 0; \
	fi; \
	if ! [ -s "$(TOOLCHAIN_ZIP)" ] || ! unzip -tq "$(TOOLCHAIN_ZIP)" >/dev/null 2>&1; then \
		printf "$(BLUE)[TOOLS]$(NC) Downloading x86_64-elf toolchain...\n"; \
		rm -f "$(TOOLCHAIN_ZIP_TMP)"; \
		if [ -t 2 ] && [ "$${TERM:-}" != dumb ]; then \
			$(CURL) $(CURL_FLAGS) --progress-bar "$(TOOLCHAIN_URL)" -o "$(TOOLCHAIN_ZIP_TMP)"; \
		else \
			$(CURL) $(CURL_FLAGS) -sS "$(TOOLCHAIN_URL)" -o "$(TOOLCHAIN_ZIP_TMP)"; \
		fi; \
		unzip -tq "$(TOOLCHAIN_ZIP_TMP)" >/dev/null; \
		mv "$(TOOLCHAIN_ZIP_TMP)" "$(TOOLCHAIN_ZIP)"; \
	else \
		printf "$(BLUE)[TOOLS]$(NC) Reusing verified toolchain archive.\n"; \
	fi; \
	printf "$(BLUE)[TOOLS]$(NC) Extracting toolchain...\n"; \
	unzip -oqq "$(TOOLCHAIN_ZIP)" -d "$(TOOLCHAIN_DIR)"; \
	test -x "$(CC)" && test -x "$(CXX)" && test -x "$(LD)"; \
	touch "$@"
endef

$(TOOLCHAIN_STAMP):
	@mkdir -p $(TOOLCHAIN_DIR)
	$(Q)if command -v flock >/dev/null 2>&1; then \
		flock "$(TOOLCHAIN_LOCK)" sh -ec '$(INSTALL_TOOLCHAIN)'; \
	else \
		sh -ec '$(INSTALL_TOOLCHAIN)'; \
	fi

$(VENV_STAMP): $(REQUIREMENTS) base.mk
	@printf "$(BLUE)[VENV]$(NC) Ensuring Python environment...\n"
	$(Q)test -d "$(VENV)" || python3 -m venv "$(VENV)"
	$(Q)PIP_DISABLE_PIP_VERSION_CHECK=1 \
		"$(VENV_PIP)" install -q -r "$(REQUIREMENTS)"
	@touch $@

$(SUBMODULE_STAMP): 
	@git submodule update --init --recursive
	@mkdir -p build
	@touch $(SUBMODULE_STAMP)

.PHONY: format
format:
	@printf "$(BLUE)[FORMAT]$(NC) Formatting C/C++ files...\n"
	$(Q)clang-format -i $(FORMAT_SOURCES)

.PHONY: check-format
check-format:
	@printf "$(BLUE)[FORMAT]$(NC) Checking formatting...\n"
	$(Q)clang-format --dry-run --Werror $(FORMAT_SOURCES)