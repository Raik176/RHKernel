include $(abspath $(TOP_DIR)/base.mk)

CFLAGS := $(GLOBAL_CFLAGS) \
            --sysroot=$(abspath $(SYSROOT)) \
            -isystem $(abspath $(SYSROOT)/include) \
            -fno-stack-protector -O2 -Iinclude
CXXFLAGS := $(CFLAGS) -fno-exceptions -fno-rtti
LDFLAGS := $(GLOBAL_LDFLAGS) -T $(abspath $(TOP_DIR)/src/user/linker.ld) -static

APP_NAME := $(shell basename $(CURDIR))

SRC_FILES := $(shell find src -name '*.cpp' -o -name '*.c' 2>/dev/null)
OBJ_FILES := $(patsubst src/%, $(TOP_DIR)/build/user/$(APP_NAME)/%.o, $(SRC_FILES))

BIN := $(USER_BINARIES_DIR)/$(APP_NAME)

.PHONY: all
all: $(BIN)

$(BIN): $(OBJ_FILES) $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	@printf "$(BLUE)[LD]$(NC) $(BIN)\n"
	$(Q)$(CXX) -nostdlib $(foreach flag,$(LDFLAGS),-Wl$\,$(flag)) $(NEWLIB_BUILD)/crt0.o $(OBJ_FILES) -L$(abspath $(SYSROOT)/lib) -lc -o $(BIN)

$(TOP_DIR)/build/user/$(APP_NAME)/%.cpp.o: src/%.cpp | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	@printf "$(GREEN)[CXX]$(NC) $<\n"
	$(Q)$(CXX) $(CXXFLAGS) -c $< -o $@

$(TOP_DIR)/build/user/$(APP_NAME)/%.c.o: src/%.c | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	@printf "$(GREEN)[CC]$(NC) $<\n"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJ_FILES:.o=.d)
