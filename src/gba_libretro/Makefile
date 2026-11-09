CXX ?= g++
CPPFLAGS := -Wall -Wextra -Wconversion -std=c++17 -O2 -fPIC \
	-Iinclude -I../libretro/include -I../gba/include -I../lib/include -I../../tests/include
build_dir ?= build
build_dir_stamp := $(build_dir)/.directory-ready

gba_sources := ../gba/arm7tdmi.cpp ../gba/gba_system.cpp ../gba/gba_audio.cpp \
	../gba/gba_cartridge_rtc.cpp \
	../lib/dmg_apu.cpp
core_info := lotusgba_libretro.info

ifeq ($(shell uname -s),Darwin)
macos_core := $(build_dir)/lotusgba_libretro.dylib
endif

.PHONY: all build test

all: build

build: $(build_dir)/lotusgba_libretro.so $(build_dir)/lotusgba_libretro.info $(macos_core)

test: $(build_dir)/gba_libretro_core_test $(build_dir)/gba_video_test \
	$(build_dir)/gba_audio_test $(build_dir)/gba_rtc_test
	$(build_dir)/gba_libretro_core_test
	$(build_dir)/gba_video_test
	$(build_dir)/gba_audio_test
	$(build_dir)/gba_rtc_test

$(build_dir)/lotusgba_libretro.so: libretro_gba_core.cpp $(gba_sources) \
		../gba/include/arm7tdmi.hpp ../gba/include/gba_system.hpp \
		../gba/include/gba_cartridge_rtc.hpp \
		../gba/include/gba_audio.hpp ../lib/include/dmg_apu.hpp \
		../libretro/include/libretro.h | $(build_dir_stamp)
	$(CXX) $(CPPFLAGS) -o $@ libretro_gba_core.cpp $(gba_sources) -shared

$(build_dir)/lotusgba_libretro.dylib: $(build_dir)/lotusgba_libretro.so
	cp $< $@

$(build_dir)/lotusgba_libretro.info: $(core_info) | $(build_dir_stamp)
	cp $< $@

$(build_dir)/gba_libretro_core_test: gba_libretro_core_test.cpp libretro_gba_core.cpp \
		$(gba_sources) ../../tests/include/gba_boot_rom.hpp \
		../../tests/roms/gba-arm-tests/arm.gba \
		../../tests/roms/gba-thumb-tests/thumb.gba \
		../../tests/roms/gba-pong/pong.gba \
		../gba/include/arm7tdmi.hpp ../gba/include/gba_system.hpp \
		../gba/include/gba_cartridge_rtc.hpp \
		../gba/include/gba_audio.hpp ../lib/include/dmg_apu.hpp \
		../libretro/include/libretro.h | $(build_dir_stamp)
	$(CXX) $(CPPFLAGS) -o $@ gba_libretro_core_test.cpp \
		libretro_gba_core.cpp $(gba_sources)

$(build_dir)/gba_video_test: gba_video_test.cpp $(gba_sources) \
		../../tests/include/gba_boot_rom.hpp \
		../gba/include/arm7tdmi.hpp ../gba/include/gba_system.hpp \
		../gba/include/gba_cartridge_rtc.hpp \
		../gba/include/gba_audio.hpp ../lib/include/dmg_apu.hpp | $(build_dir_stamp)
	$(CXX) $(CPPFLAGS) -o $@ gba_video_test.cpp $(gba_sources)

$(build_dir)/gba_audio_test: gba_audio_test.cpp $(gba_sources) \
		../../tests/roms/gba-audio-check/audio-check.gba \
		../../tests/roms/gba-audio-check/direct-sound-check.gba \
		../gba/include/arm7tdmi.hpp ../gba/include/gba_system.hpp \
		../gba/include/gba_cartridge_rtc.hpp \
		../gba/include/gba_audio.hpp ../lib/include/dmg_apu.hpp | $(build_dir_stamp)
	$(CXX) $(CPPFLAGS) -o $@ gba_audio_test.cpp $(gba_sources)

../../tests/roms/gba-audio-check/audio-check.gba: \
		../../tests/roms/gba-audio-check/generate.py
	python3 $<

../../tests/roms/gba-audio-check/direct-sound-check.gba: \
		../../tests/roms/gba-audio-check/generate.py
	python3 $<

$(build_dir)/gba_rtc_test: gba_rtc_test.cpp ../gba/gba_system.cpp \
		../gba/gba_cartridge_rtc.cpp ../gba/arm7tdmi.cpp \
		../gba/gba_audio.cpp ../lib/dmg_apu.cpp \
		../gba/include/arm7tdmi.hpp ../gba/include/gba_system.hpp \
		../gba/include/gba_cartridge_rtc.hpp ../gba/include/gba_audio.hpp \
		../lib/include/dmg_apu.hpp | $(build_dir_stamp)
	$(CXX) $(CPPFLAGS) -o $@ gba_rtc_test.cpp ../gba/arm7tdmi.cpp \
		../gba/gba_system.cpp ../gba/gba_cartridge_rtc.cpp \
		../gba/gba_audio.cpp ../lib/dmg_apu.cpp

$(build_dir_stamp):
	mkdir -p $(build_dir)
	touch $@
