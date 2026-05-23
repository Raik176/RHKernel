include $(abspath $(TOP_DIR)/base.mk)

MODULE_INC_DIRS := $(shell find $(abspath $(TOP_DIR)/src/module) -type d -name "include" 2>/dev/null)
MODULE_INC_FLAGS := $(addprefix -I, $(MODULE_INC_DIRS))

CFLAGS := $(GLOBAL_CFLAGS) -ffreestanding -fno-stack-protector -O2 \
			-fno-function-sections -fno-data-sections \
            -fPIC -fno-pie -fno-plt -mcmodel=large -fno-lto \
            -I$(abspath $(TOP_DIR)/src/public) \
            $(MODULE_INC_FLAGS)

CXXFLAGS := $(CFLAGS) -fno-exceptions -fno-rtti
LDFLAGS := -static -r

APP_NAME := $(shell basename $(CURDIR))

SRC_FILES := $(shell find src -name '*.cpp' -o -name '*.c' 2>/dev/null)
OBJ_FILES := $(patsubst src/%, $(TOP_DIR)/build/modules/$(APP_NAME)/%.o, $(SRC_FILES))

KO := $(MODULE_BINARIES_DIR)/$(APP_NAME).ko
APP_BUILD_CFG := $(TOP_DIR)/build/modules/$(APP_NAME)/.build.cfg

.PHONY: all FORCE
FORCE:

all: $(TOOLCHAIN_STAMP) $(KO)

$(APP_BUILD_CFG): FORCE
	@mkdir -p $(dir $@)
	$(Q){ \
		printf 'CFLAGS=%s\n' '$(CFLAGS)'; \
		printf 'CXXFLAGS=%s\n' '$(CXXFLAGS)'; \
		printf 'LDFLAGS=%s\n' '$(LDFLAGS)'; \
	} > $@.tmp
	$(Q)if test -r $@ && cmp -s $@ $@.tmp; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(KO): $(OBJ_FILES) $(APP_BUILD_CFG) $(TOOLCHAIN_STAMP) $(TOP_DIR)/src/module/common.mk $(TOP_DIR)/base.mk
	@mkdir -p $(dir $@)
	@printf "$(BLUE)[LD]$(NC) $(APP_NAME).ko\n"
	$(Q)$(LD) $(LDFLAGS) $(OBJ_FILES) -o $(KO)

$(TOP_DIR)/build/modules/$(APP_NAME)/%.cpp.o: src/%.cpp $(APP_BUILD_CFG) $(TOP_DIR)/src/module/common.mk $(TOP_DIR)/base.mk | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	@printf "$(GREEN)[CXX]$(NC) $<\n"
	$(Q)$(CXX) $(DEPFLAGS) $(CXXFLAGS) -c $< -o $@

$(TOP_DIR)/build/modules/$(APP_NAME)/%.c.o: src/%.c $(APP_BUILD_CFG) $(TOP_DIR)/src/module/common.mk $(TOP_DIR)/base.mk | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	@printf "$(GREEN)[CC]$(NC) $<\n"
	$(Q)$(CC) $(DEPFLAGS) $(CFLAGS) -c $< -o $@

-include $(OBJ_FILES:.o=.d)
