include $(abspath $(TOP_DIR)/base.mk)

CFLAGS := $(GLOBAL_CFLAGS) \
            -ffreestanding -nostdinc \
            -isystem $(abspath $(SYSROOT)/include) \
            $(USER_RUNTIME_CFLAGS) -iquote include
CXXFLAGS := $(CFLAGS) -fexceptions -frtti
LDFLAGS := $(GLOBAL_LDFLAGS) --eh-frame-hdr -T $(abspath $(TOP_DIR)/src/user/linker.ld) -static -pie

USER_SRC_ROOT := $(abspath $(TOP_DIR)/src/user)
APP_REL_PATH := $(patsubst $(USER_SRC_ROOT)/%,%,$(abspath $(CURDIR)))
APP_NAME := $(notdir $(APP_REL_PATH))
APP_BUILD_DIR := $(TOP_DIR)/build/user/$(APP_REL_PATH)

SRC_FILES := $(shell find src -name '*.cpp' -o -name '*.c' 2>/dev/null)
OBJ_FILES := $(patsubst src/%, $(APP_BUILD_DIR)/%.o, $(SRC_FILES))

BIN := $(USER_BINARIES_DIR)/$(APP_REL_PATH)
APP_BUILD_CFG := $(APP_BUILD_DIR)/.build.cfg

.PHONY: all FORCE
FORCE:

all: $(BIN)

$(APP_BUILD_CFG): FORCE
	@mkdir -p $(dir $@)
	$(Q){ \
		printf 'CFLAGS=%s\n' '$(CFLAGS)'; \
		printf 'CXXFLAGS=%s\n' '$(CXXFLAGS)'; \
		printf 'LDFLAGS=%s\n' '$(LDFLAGS)'; \
	} > $@.tmp
	$(Q)if test -r $@ && cmp -s $@ $@.tmp; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(BIN): $(OBJ_FILES) $(APP_BUILD_CFG) $(TOOLCHAIN_STAMP) $(LIBC_CRT0) $(SYSROOT)/lib/libc.a $(SYSROOT)/lib/libcppabi.a $(TOP_DIR)/src/user/linker.ld $(TOP_DIR)/src/user/common.mk $(TOP_DIR)/base.mk
	@mkdir -p $(dir $@)
	@printf "$(BLUE)[LD]$(NC) $(BIN)\n"
	$(Q)$(CXX) -nostdlib $(foreach flag,$(LDFLAGS),-Wl$\,$(flag)) $(LIBC_CRT0) $(OBJ_FILES) -L$(abspath $(SYSROOT)/lib) -Wl,--start-group -lcppabi -lc -lgcc -Wl,--end-group -o $(BIN)

$(APP_BUILD_DIR)/%.cpp.o: src/%.cpp $(APP_BUILD_CFG) $(TOP_DIR)/src/user/common.mk $(TOP_DIR)/base.mk | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	@printf "$(GREEN)[CXX]$(NC) $<\n"
	$(Q)$(CXX) $(DEPFLAGS) $(CXXFLAGS) -c $< -o $@

$(APP_BUILD_DIR)/%.c.o: src/%.c $(APP_BUILD_CFG) $(TOP_DIR)/src/user/common.mk $(TOP_DIR)/base.mk | $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	@printf "$(GREEN)[CC]$(NC) $<\n"
	$(Q)$(CC) $(DEPFLAGS) $(CFLAGS) -c $< -o $@

-include $(OBJ_FILES:.o=.d)
