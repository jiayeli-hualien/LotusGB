CXX ?= g++
CPPFLAGS := -Wall -Wextra -std=c++17 -O2 -fPIC -Iinclude -I../lib/include
build_dir ?= build

libretro_header := include/libretro.h
libretro_headers := $(wildcard include/*.hpp)
generated_tables := ../lib/include/generated_decode_tables.hpp
core_info := lotusgb_libretro.info
csv_sources := $(wildcard ../lib/include/*.csv)
core_headers := $(wildcard ../lib/include/*.hpp)
core_sources := $(filter-out ../lib/GBITCPU.cpp ../lib/lotusGBIT.cpp \
	../lib/lotus_gbit_c_wrapper.cpp ../lib/memoryGBIT.cpp,$(wildcard ../lib/*.cpp))
asset_dir := ../../tests/roms
test_fixtures := $(wildcard ../../tests/include/*.hpp)

ifeq ($(shell uname -s),Darwin)
macos_core := $(build_dir)/lotusgb_libretro.dylib
endif

.PHONY: all build test test-vblank rom-test-runner

all: build

build: $(build_dir)/lotusgb_libretro.so $(build_dir)/lotusgb_libretro.info $(macos_core)

test: $(build_dir)/libretro_core_test $(build_dir)/libretro_fixed_input_test
	$(build_dir)/libretro_core_test
	$(build_dir)/libretro_fixed_input_test

test-vblank: $(build_dir)/libretro_core_test
	$(build_dir)/libretro_core_test

rom-test-runner: $(build_dir)/libretro_rom_runner

$(generated_tables): $(csv_sources) ../../tools/generate_decode_tables.py
	python3 ../../tools/generate_decode_tables.py

$(build_dir)/lotusgb_libretro.so: libretro_core.cpp $(core_sources) \
		$(core_headers) $(libretro_header) $(libretro_headers) $(generated_tables) | $(build_dir)
	$(CXX) $(CPPFLAGS) -o $@ libretro_core.cpp $(core_sources) -shared

$(build_dir)/lotusgb_libretro.dylib: $(build_dir)/lotusgb_libretro.so
	cp $< $@

$(build_dir)/lotusgb_libretro.info: $(core_info) | $(build_dir)
	cp $< $@

$(build_dir)/libretro_core_test: libretro_core_test.cpp libretro_core.cpp \
		$(core_sources) $(core_headers) $(libretro_header) $(libretro_headers) $(generated_tables) $(test_fixtures) | $(build_dir)
	$(CXX) $(CPPFLAGS) -DLOTUSGB_LIBRETRO_TEST_TIMEOUT_MS=250 \
		-DLOTUSGB_LIBRETRO_TEST_ASSET_DIR=\"$(asset_dir)\" \
		-o $@ libretro_core_test.cpp libretro_core.cpp $(core_sources)

# Fixed-step diagnostic executable, never installed as the ordinary core.
$(build_dir)/libretro_fixed_input_test: libretro_core_test.cpp libretro_core.cpp \
		$(core_sources) $(core_headers) $(libretro_header) $(libretro_headers) $(generated_tables) $(test_fixtures) | $(build_dir)
	$(CXX) $(CPPFLAGS) -DLOTUSGB_TEST_FIXED_FRAME_STEP \
		-DLOTUSGB_LIBRETRO_TEST_TIMEOUT_MS=250 \
		-DLOTUSGB_LIBRETRO_TEST_ASSET_DIR=\"$(asset_dir)\" \
		-o $@ libretro_core_test.cpp libretro_core.cpp $(core_sources)

$(build_dir)/libretro_rom_runner: libretro_rom_runner.cpp $(libretro_header) | $(build_dir)
	$(CXX) $(CPPFLAGS) -o $@ libretro_rom_runner.cpp -ldl

$(build_dir):
	mkdir -p $@
