CXX ?= g++
CPPFLAGS := -Wall -Wextra -std=c++17 -O2 -fPIC -Iinclude -I../lib/include
build_dir ?= build

libretro_header := include/libretro.h
generated_tables := ../lib/include/generated_decode_tables.hpp
csv_sources := $(wildcard ../lib/include/*.csv)
core_headers := $(wildcard ../lib/include/*.hpp)
core_sources := $(filter-out ../lib/GBITCPU.cpp ../lib/lotusGBIT.cpp \
	../lib/lotus_gbit_c_wrapper.cpp ../lib/memoryGBIT.cpp,$(wildcard ../lib/*.cpp))
asset_dir := ../../tests/roms

.PHONY: all build test rom-test-runner

all: build

build: $(build_dir)/lotusgb_libretro.so

test: $(build_dir)/libretro_core_test
	$(build_dir)/libretro_core_test

rom-test-runner: $(build_dir)/libretro_rom_runner

$(generated_tables): $(csv_sources) ../../tools/generate_decode_tables.py
	python3 ../../tools/generate_decode_tables.py

$(build_dir)/lotusgb_libretro.so: libretro_core.cpp $(core_sources) \
		$(core_headers) $(libretro_header) $(generated_tables) | $(build_dir)
	$(CXX) $(CPPFLAGS) -o $@ libretro_core.cpp $(core_sources) -shared

$(build_dir)/libretro_core_test: libretro_core_test.cpp libretro_core.cpp \
		$(core_sources) $(core_headers) $(libretro_header) $(generated_tables) | $(build_dir)
	$(CXX) $(CPPFLAGS) -DLOTUSGB_LIBRETRO_TEST_ASSET_DIR=\"$(asset_dir)\" \
		-o $@ libretro_core_test.cpp libretro_core.cpp $(core_sources)

$(build_dir)/libretro_rom_runner: libretro_rom_runner.cpp $(libretro_header) | $(build_dir)
	$(CXX) $(CPPFLAGS) -o $@ libretro_rom_runner.cpp -ldl

$(build_dir):
	mkdir -p $@
