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

USER_PIE_CFLAGS := -fPIE
USER_RUNTIME_CFLAGS := $(USER_PIE_CFLAGS)
NEWLIB_TARGET_CFLAGS := $(USER_RUNTIME_CFLAGS)

# Dependency and safety helpers used by every Makefile.  Keep dependency files
# next to their object files and make the generated .d target match the real
# object path, so moving sources under nested directories stays correct.
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

NEWLIB_SRC := $(TOP_DIR)/third_party/newlib
NEWLIB_BUILD := $(TOP_DIR)/build/third_party/newlib
NEWLIB_CONFIG := $(TOP_DIR)/build/.newlib-build.cfg
NEWLIB_PREFIX := $(abspath $(SYSROOT)/..)
NEWLIB_STUB_BUILD := $(TOP_DIR)/build/newlib-stubs
NEWLIB_STUB_SRC_FILES := $(shell find $(TOP_DIR)/src/newlib -name '*.c' 2>/dev/null)
NEWLIB_STUB_OBJ_FILES := $(patsubst $(TOP_DIR)/src/newlib/%.c, $(NEWLIB_STUB_BUILD)/%.o, $(NEWLIB_STUB_SRC_FILES))
NEWLIB_CRT0 := $(NEWLIB_STUB_BUILD)/crt0.o
NEWLIB_STUB_LIB_OBJ_FILES := $(filter-out $(NEWLIB_CRT0),$(NEWLIB_STUB_OBJ_FILES))
NEWLIB_STAMP := $(NEWLIB_BUILD)/.newlib_done

$(NEWLIB_STUB_BUILD)/%.o: $(TOP_DIR)/src/newlib/%.c $(TOP_DIR)/base.mk | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	$(Q)$(CC) --sysroot=$(abspath $(SYSROOT)) \
		-isystem $(abspath $(SYSROOT)/include) \
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