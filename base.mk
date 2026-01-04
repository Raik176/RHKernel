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

TOOLS_DIR := $(TOP_DIR)/tools
CC  := $(TOOLS_DIR)/bin/x86_64-elf-gcc
CXX := $(TOOLS_DIR)/bin/x86_64-elf-g++
LD  := $(TOOLS_DIR)/bin/x86_64-elf-ld
SYSROOT := $(TOP_DIR)/build/sysroot/x86_64-elf

ifeq ($(V),1)
    Q :=
else
    Q := @
endif

GLOBAL_CFLAGS := -m64 -MMD -MP -Wall -Wextra