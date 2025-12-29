CC  := $(abspath $(TOP_DIR)/tools/bin/x86_64-elf-gcc)
CXX := $(abspath $(TOP_DIR)/tools/bin/x86_64-elf-g++)
LD  := $(abspath $(TOP_DIR)/tools/bin/x86_64-elf-ld)

CFLAGS   := -ffreestanding -fno-stack-protector -m64 -O2 -Iinclude -I$(abspath $(TOP_DIR)/src/user/libc/include)
CXXFLAGS := $(CFLAGS) -fno-exceptions -fno-rtti
LDFLAGS  := -n -T $(abspath $(TOP_DIR)/src/user/linker.ld) -static

# Automatic discovery of source files
SRC_FILES := $(shell find src -name '*.cpp' -o -name '*.c')
OBJ_FILES := $(patsubst src/%, build/%.o, $(SRC_FILES))

APP_NAME := $(shell basename $(CURDIR))
BIN := bin/$(APP_NAME)

all: $(BIN)

$(BIN): $(OBJ_FILES)
	@mkdir -p bin
	$(LD) $(LDFLAGS) $(OBJ_FILES) -o $(BIN)

build/%.cpp.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/%.c.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf bin build