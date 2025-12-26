CC  := ../tools/bin/x86_64-elf-gcc
CXX := ../tools/bin/x86_64-elf-g++
LD  := ../tools/bin/x86_64-elf-ld

USER_CXXFLAGS := -ffreestanding -fno-stack-protector -fno-exceptions -fno-rtti -m64 -O2
USER_LDFLAGS  := -n -T linker.ld -static

all: bin/init

bin/init: src/init.cpp
	@mkdir -p bin
	$(CXX) $(USER_CXXFLAGS) -c src/init.cpp -o bin/init.o
	$(LD) $(USER_LDFLAGS) bin/init.o -o bin/init

clean:
	rm -rf bin