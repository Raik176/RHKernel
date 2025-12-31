CC  := $(abspath $(TOP_DIR)/tools/bin/x86_64-elf-gcc)
CXX := $(abspath $(TOP_DIR)/tools/bin/x86_64-elf-g++)
LD  := $(abspath $(TOP_DIR)/tools/bin/x86_64-elf-ld)

MODULE_INC_DIRS := $(shell find $(abspath $(TOP_DIR)/src/module) -type d -name "include" 2>/dev/null)
MODULE_INC_FLAGS := $(addprefix -I, $(MODULE_INC_DIRS))

CFLAGS   := -ffreestanding -fno-stack-protector -m64 -O2 \
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
	$(LD) $(LDFLAGS) $(OBJ_FILES) -o $(KO)

build/%.cpp.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/%.c.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf bin build