TOP_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

ifneq ($(shell test "$$(tput colors 2>/dev/null)" -ge 8 && echo yes),)
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

GLOBAL_CFLAGS  := -m64 -MMD -MP -Wall -Wextra -flto -ffunction-sections -fdata-sections
GLOBAL_LDFLAGS := --gc-sections

ifeq ($(V),1)
    Q :=
else
    Q := @
endif

TOOLS_DIR := $(TOP_DIR)/tools
TOOLCHAIN_DIR := $(TOOLS_DIR)/x86_64-elf-toolchain
TOOLCHAIN_ZIP := $(TOOLCHAIN_DIR)/tools.zip
TOOLCHAIN_URL := https://github.com/lordmilko/i686-elf-tools/releases/download/15.2.0/x86_64-elf-tools-linux.zip
TOOLCHAIN_STAMP := $(TOOLCHAIN_DIR)/.installed

VENV ?= $(TOOLS_DIR)/.venv
VENV_PYTHON := $(VENV)/bin/python3
VENV_PIP := $(VENV)/bin/pip
VENV_STAMP := $(VENV)/.installed
REQUIREMENTS := $(TOOLS_DIR)/requirements.txt

NEWLIB_SRC := $(TOP_DIR)/third_party/newlib
NEWLIB_BUILD := $(TOP_DIR)/build/newlib
NEWLIB_STUB_SRC_FILES := $(shell find $(TOP_DIR)/src/newlib -name '*.c' 2>/dev/null)
NEWLIB_STUB_OBJ_FILES := $(patsubst $(TOP_DIR)/src/newlib/%.c, build/newlib/%.o, $(NEWLIB_STUB_SRC_FILES))
NEWLIB_STAMP := $(NEWLIB_BUILD)/.newlib_done

CC := $(TOOLCHAIN_DIR)/bin/x86_64-elf-gcc
CXX := $(TOOLCHAIN_DIR)/bin/x86_64-elf-g++
LD := $(TOOLCHAIN_DIR)/bin/x86_64-elf-ld
AS := $(TOOLCHAIN_DIR)/bin/x86_64-elf-as
AR := $(TOOLCHAIN_DIR)/bin/x86_64-elf-ar
RANLIB := $(TOOLCHAIN_DIR)/bin/x86_64-elf-ranlib

MODULE_BINARIES_DIR := $(TOP_DIR)/build/bin/modules
USER_BINARIES_DIR := $(TOP_DIR)/build/bin/user

$(TOOLCHAIN_STAMP):
	@mkdir -p $(TOOLCHAIN_DIR)
	@echo -e "$(BLUE)[TOOLS]$(NC) Downloading x86_64-elf toolchain..."
	$(Q)curl -fsSL $(TOOLCHAIN_URL) -o $(TOOLCHAIN_ZIP)
	@echo -e "$(BLUE)[TOOLS]$(NC) Extracting..."
	$(Q)unzip -oqq $(TOOLCHAIN_ZIP) -d $(TOOLCHAIN_DIR)
	@touch $@

$(VENV_STAMP): $(REQUIREMENTS)
	@echo -e "$(BLUE)[VENV]$(NC) Ensuring Python environment..."
	$(Q)test -d $(VENV) || python3 -m venv $(VENV)
	$(Q)PIP_DISABLE_PIP_VERSION_CHECK=1 \
		$(VENV_PIP) install -q -r $(REQUIREMENTS)
	@touch $@

.PHONY: format
format:
	@echo -e "$(BLUE)[FORMAT]$(NC) Formatting C/C++ files..."
	$(Q)clang-format -i $(FORMAT_SOURCES)

.PHONY: check-format
check-format:
	@echo -e "$(BLUE)[FORMAT]$(NC) Checking formatting..."
	$(Q)clang-format --dry-run --Werror $(FORMAT_SOURCES)