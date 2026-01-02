include $(abspath $(TOP_DIR)/base.mk)

CFLAGS   := $(GLOBAL_CFLAGS) -ffreestanding -fno-stack-protector -O2 -Iinclude
CXXFLAGS := $(CFLAGS) -fno-exceptions -fno-rtti
LDFLAGS  := -n -T $(abspath $(TOP_DIR)/src/user/linker.ld) -static

SRC_FILES := $(shell find src -name '*.cpp' -o -name '*.c')
OBJ_FILES := $(patsubst src/%, build/%.o, $(SRC_FILES))

APP_NAME := $(shell basename $(CURDIR))
BIN      := bin/$(APP_NAME)

all: $(BIN)

$(BIN): $(OBJ_FILES)
	@mkdir -p bin
	@echo -e "$(BLUE)[LD]$(NC) $(BIN)"
	$(Q)$(LD) $(LDFLAGS) $(OBJ_FILES) -o $(BIN)

build/%.cpp.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo -e "$(GREEN)[CXX]$(NC) $<"
	$(Q)$(CXX) $(CXXFLAGS) -c $< -o $@

build/%.c.o: src/%.c
	@mkdir -p $(dir $@)
	@echo -e "$(GREEN)[CC]$(NC) $<"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(Q)rm -rf bin build

-include $(OBJ_FILES:.o=.d)