CXX ?= g++
CPPFLAGS := -Wall -Wextra -Wconversion -std=c++17 -O2 -fPIC \
	-Iinclude -I../libretro/include -I../gba/include -I../../tests/include
build_dir ?= build

gba_sources := ../gba/arm7tdmi.cpp ../gba/gba_system.cpp
core_info := lotusgba_libretro.info

ifeq ($(shell uname -s),Darwin)
macos_core := $(build_dir)/lotusgba_libretro.dylib
endif

.PHONY: all build test

all: build

build: $(build_dir)/lotusgba_libretro.so $(build_dir)/lotusgba_libretro.info $(macos_core)

test: $(build_dir)/gba_libretro_core_test
	$(build_dir)/gba_libretro_core_test

$(build_dir)/lotusgba_libretro.so: libretro_gba_core.cpp $(gba_sources) \
		../gba/include/arm7tdmi.hpp ../gba/include/gba_system.hpp ../libretro/include/libretro.h | $(build_dir)
	$(CXX) $(CPPFLAGS) -o $@ libretro_gba_core.cpp $(gba_sources) -shared

$(build_dir)/lotusgba_libretro.dylib: $(build_dir)/lotusgba_libretro.so
	cp $< $@

$(build_dir)/lotusgba_libretro.info: $(core_info) | $(build_dir)
	cp $< $@

$(build_dir)/gba_libretro_core_test: gba_libretro_core_test.cpp libretro_gba_core.cpp \
		$(gba_sources) ../../tests/include/gba_boot_rom.hpp \
		../../tests/roms/gba-arm-tests/arm.gba \
		../../tests/roms/gba-thumb-tests/thumb.gba \
		../../tests/roms/gba-pong/pong.gba \
		../gba/include/arm7tdmi.hpp ../gba/include/gba_system.hpp \
		../libretro/include/libretro.h | $(build_dir)
	$(CXX) $(CPPFLAGS) -o $@ gba_libretro_core_test.cpp \
		libretro_gba_core.cpp $(gba_sources)

$(build_dir):
	mkdir -p $@
