include $(abspath $(TOP_DIR)/base.mk)

MODULE_INC_DIRS := $(shell find $(abspath $(TOP_DIR)/src/module) -type d -name "include" 2>/dev/null)
MODULE_INC_FLAGS := $(addprefix -I, $(MODULE_INC_DIRS))

CFLAGS   := $(GLOBAL_CFLAGS) -ffreestanding -fno-stack-protector -O2 \
            -fPIC -fno-pie -fno-plt -mcmodel=large \
            -I$(abspath $(TOP_DIR)/src/public) \
            $(MODULE_INC_FLAGS)

CXXFLAGS := $(CFLAGS) -fno-exceptions -fno-rtti
LDFLAGS  := -static -r

SRC_FILES := $(shell find src -name '*.cpp' -o -name '*.c')
OBJ_FILES := $(patsubst src/%, build/%.o, $(SRC_FILES))

APP_NAME := $(shell basename $(CURDIR))
KO := bin/$(APP_NAME).ko

all: $(KO)

$(KO): $(OBJ_FILES)
	@mkdir -p bin
	@echo -e "$(BLUE)[LD]$(NC) $(APP_NAME).ko"
	$(Q)$(LD) $(LDFLAGS) $(OBJ_FILES) -o $(KO)

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