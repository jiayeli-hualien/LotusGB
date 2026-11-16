# Let src/Makefile handle all builds, pass some variables

src_dir := $(CURDIR)/src
HOST_OS := $(shell uname -s)
HOST_ARCH := $(shell uname -m)
ifeq ($(HOST_OS)-$(HOST_ARCH),Darwin-arm64)
HOST_TARGET := mac-m1
else ifeq ($(HOST_OS)-$(HOST_ARCH),Linux-x86_64)
HOST_TARGET := ubuntu-x86_64
else
$(error Unsupported host $(HOST_OS)-$(HOST_ARCH); supported hosts are macOS arm64 and Linux x86_64)
endif
TARGET ?= $(HOST_TARGET)
ifeq ($(TARGET),$(HOST_TARGET))
build_dir ?= $(CURDIR)/build
else ifeq ($(TARGET),steamdeck)
build_dir ?= $(CURDIR)/build/$(TARGET)
else ifneq ($(filter $(TARGET),ubuntu-x86_64 mac-m1),)
$(error TARGET=$(TARGET) is a native build; current host is $(HOST_TARGET). Cross-compilation is enabled only for TARGET=steamdeck)
else
$(error Unsupported TARGET '$(TARGET)'; choose ubuntu-x86_64, mac-m1, or steamdeck)
endif

# PHONY: non file targets
.PHONY: build test test-roms test-mooneye test-mooneye-visual test-homebrew test-cgb-homebrew \
	test-gbmicrotest-ppu-latch test-tobu-input test-tobu-input-vblank test-tobu-input-fixed test-vblank test-tobudx install-retroarch-core-macos package-retroarch-macos package-retroarch-steam-deck clean

TEST_ROM_DIR ?= ../test-roms/dmg_sound
TEST_ROM_FRAMES ?= 1200
HOMEBREW_GAME ?= big2small
homebrew_rom_big2small := ../test-roms/homebrew/big2small.gb
homebrew_rom_tobutobugirl := ../test-roms/homebrew/tobutobugirl/tobu.gb
homebrew_rom_canyon-racer := ../test-roms/homebrew/canyon-racer/canyon_racer_0.7.7_mbc5.gb
homebrew_rom_adjustris := ../test-roms/homebrew/Adjustris-source/adjustris.gb
HOMEBREW_ROM ?= $(homebrew_rom_$(HOMEBREW_GAME))
HOMEBREW_SAVE ?=
MOONEYE_GROUP ?= ppu
MOONEYE_OUTPUT_DIR ?= $(build_dir)/test-roms/mooneye$(if $(filter ppu,$(MOONEYE_GROUP)),,-$(MOONEYE_GROUP))
MOONEYE_SUITE_DIR ?= ../test-roms/mooneye
MOONEYE_CGB_BOOT_ROM ?=
MOONEYE_CGB_BOOT_ROM_ARGS = $(if $(MOONEYE_CGB_BOOT_ROM),--cgb-boot-rom "$(MOONEYE_CGB_BOOT_ROM)")
MOONEYE_VISUAL_SUITE_DIR ?= ../test-roms/game-boy-test-roms-v7.0/mooneye-test-suite
VISUAL_TEST_PYTHON ?= $(build_dir)/visual-venv/bin/python
GBMICROTEST_SUITE_DIR ?= ../test-roms/gbmicrotest
RETROARCH_CORE_DIR ?= $(HOME)/Library/Application Support/RetroArch/cores
RETROARCH_INFO_DIR ?= $(HOME)/Library/Application Support/RetroArch/info

all: build

$(build_dir):
	mkdir -p $@

ifeq ($(TARGET),$(HOST_TARGET))
build: $(build_dir)
	$(MAKE) -C $(src_dir) build_dir=$(build_dir)
else
build: $(build_dir)
	@test -x "$(STEAM_DECK_ZIG)" || { printf '%s\n' 'Zig 0.17.0 not found; set STEAM_DECK_ZIG.' >&2; exit 1; }
	ZIG_GLOBAL_CACHE_DIR="$(CURDIR)/build/zig-cache/global" \
	ZIG_LOCAL_CACHE_DIR="$(CURDIR)/build/zig-cache/local" \
	$(MAKE) -C $(src_dir)/libretro build_dir="$(build_dir)" \
		CXX="$(STEAM_DECK_ZIG) c++ -target $(STEAM_DECK_TARGET)" \
		"$(build_dir)/lotusgb_libretro.so" "$(build_dir)/lotusgb_libretro.info"
endif

install-retroarch-core-macos: $(build_dir)
	@test "$$(uname -s)" = Darwin || { printf '%s\n' 'This target requires macOS.' >&2; exit 1; }
	$(MAKE) -C $(src_dir)/libretro build build_dir=$(build_dir)
	@test -d "$(RETROARCH_CORE_DIR)" || { printf '%s\n' 'RetroArch core directory not found; set RETROARCH_CORE_DIR.' >&2; exit 1; }
	@test -d "$(RETROARCH_INFO_DIR)" || { printf '%s\n' 'RetroArch info directory not found; set RETROARCH_INFO_DIR.' >&2; exit 1; }
	@set -eu; \
		lotus_core_tmp="$$(mktemp "$(RETROARCH_CORE_DIR)/.lotusgb_libretro.XXXXXXXX")"; \
		lotus_info_tmp=""; \
		trap 'rm -f "$$lotus_core_tmp" "$$lotus_info_tmp"' EXIT; \
		lotus_info_tmp="$$(mktemp "$(RETROARCH_INFO_DIR)/.lotusgb_libretro.XXXXXXXX")"; \
		cp "$(build_dir)/lotusgb_libretro.dylib" "$$lotus_core_tmp"; \
		cp "$(build_dir)/lotusgb_libretro.info" "$$lotus_info_tmp"; \
		chmod 755 "$$lotus_core_tmp"; chmod 644 "$$lotus_info_tmp"; \
		mv -f "$$lotus_core_tmp" "$(RETROARCH_CORE_DIR)/lotusgb_libretro.dylib"; \
		mv -f "$$lotus_info_tmp" "$(RETROARCH_INFO_DIR)/lotusgb_libretro.info"; \
		touch "$(RETROARCH_INFO_DIR)/core_info.refresh"
	@printf '%s\n' 'Installed LotusGB core and .info (.gb/.gbc). Restart RetroArch to load them.'

test: $(build_dir)
	@test "$(TARGET)" = "$(HOST_TARGET)" || { printf '%s\n' 'Tests run on the local host; cross-target builds are compile/package only.' >&2; exit 1; }
	$(MAKE) -C $(src_dir) test build_dir=$(build_dir)
	$(MAKE) -C $(src_dir)/lib mooneye-runner build_dir=$(build_dir)
	python3 tools/test_mooneye_runner.py --runner "$(build_dir)/mooneye_rom_runner"
	python3 tools/test_homebrew_check.py
	python3 tools/test_serial_link_timing.py
	$(MAKE) -C $(src_dir)/lib ppu-frame-runner build_dir=$(build_dir)
	python3 tools/test_ppu_frame_runner.py --runner "$(build_dir)/ppu_frame_runner"

test-roms:
	$(MAKE) -C $(src_dir)/libretro build build_dir=$(build_dir)
	$(MAKE) -C $(src_dir)/libretro rom-test-runner build_dir=$(build_dir)
	python3 tools/run_blargg_apu_tests.py \
		--core "$(build_dir)/lotusgb_libretro.so" \
		--runner "$(build_dir)/libretro_rom_runner" \
		--rom-dir "$(TEST_ROM_DIR)" \
		--frames "$(TEST_ROM_FRAMES)" \
		--output-root "$(build_dir)/test-roms"

test-mooneye:
	$(MAKE) -C $(src_dir)/lib mooneye-runner build_dir=$(build_dir)
	python3 tools/run_mooneye_ppu_tests.py \
		--runner "$(build_dir)/mooneye_rom_runner" \
		--suite-dir "$(MOONEYE_SUITE_DIR)" \
		--group "$(MOONEYE_GROUP)" \
		--output-dir "$(MOONEYE_OUTPUT_DIR)" $(MOONEYE_CGB_BOOT_ROM_ARGS)

test-mooneye-visual:
	$(MAKE) -C $(src_dir)/lib ppu-frame-runner build_dir=$(build_dir)
	$(VISUAL_TEST_PYTHON) tools/run_mooneye_sprite_priority.py \
		--runner "$(build_dir)/ppu_frame_runner" \
		--suite-dir "$(MOONEYE_VISUAL_SUITE_DIR)" \
		--output-dir "$(build_dir)/test-roms/mooneye-sprite-priority"

test-homebrew:
	$(MAKE) -C $(src_dir)/libretro build rom-test-runner build_dir=$(build_dir)
	python3 tools/run_homebrew_check.py \
		--game "$(HOMEBREW_GAME)" \
		--core "$(build_dir)/lotusgb_libretro.so" \
		--runner "$(build_dir)/libretro_rom_runner" \
		--rom "$(HOMEBREW_ROM)" \
		$(if $(HOMEBREW_SAVE),--save "$(HOMEBREW_SAVE)")

test-cgb-homebrew:
	$(MAKE) test-homebrew HOMEBREW_GAME=big2small
	$(MAKE) test-tobudx

test-tobu-input:
	$(MAKE) -C $(src_dir)/libretro $(build_dir)/libretro_core_test build_dir=$(build_dir)
	python3 tools/measure_tobu_input.py --rom "$(homebrew_rom_tobutobugirl)" \
		--test-executable "$(build_dir)/libretro_core_test"

test-tobu-input-vblank:
	$(MAKE) -C $(src_dir)/libretro $(build_dir)/libretro_core_test build_dir=$(build_dir)
	python3 tools/measure_tobu_input.py --rom "$(homebrew_rom_tobutobugirl)" \
		--test-executable "$(build_dir)/libretro_core_test" \
		--output-dir "$(build_dir)/test-roms/tobu-input-vblank"

test-tobu-input-fixed:
	$(MAKE) -C $(src_dir)/libretro $(build_dir)/libretro_fixed_input_test build_dir=$(build_dir)
	python3 tools/measure_tobu_input.py --rom "$(homebrew_rom_tobutobugirl)" \
		--test-executable "$(build_dir)/libretro_fixed_input_test" \
		--output-dir "$(build_dir)/test-roms/tobu-input-fixed"

test-vblank:
	$(MAKE) -C $(src_dir)/libretro test-vblank build_dir=$(build_dir)

test-gbmicrotest-ppu-latch:
	$(MAKE) -C $(src_dir)/libretro build rom-test-runner build_dir=$(build_dir)
	python3 tools/run_gbmicrotest_ppu_latches.py \
		--suite-dir "$(GBMICROTEST_SUITE_DIR)" \
		--core "$(build_dir)/lotusgb_libretro.so" \
		--runner "$(build_dir)/libretro_rom_runner"

clean:
	rm $(build_dir) -r

TOBUDX_ROM ?= ../test-roms/homebrew/tobutobugirl-dx/tobudx.gb
test-tobudx:
	$(MAKE) -C $(src_dir)/libretro $(build_dir)/libretro_core_test build_dir=$(build_dir)
	python3 tools/measure_tobu_input.py --game tobutobugirl-dx --rom "$(TOBUDX_ROM)" \
		--test-executable "$(build_dir)/libretro_core_test" --output-dir "$(build_dir)/test-roms/tobudx-input"

MACOS_PACKAGE_BUILD_DIR ?= $(build_dir)/macos-retroarch-universal
MACOS_PACKAGE_DIR ?= $(build_dir)/packages
MACOS_PACKAGE_FLAGS := -Wall -Wextra -std=c++17 -O2 -fPIC -Iinclude -I../lib/include -arch arm64 -arch x86_64 -mmacosx-version-min=11.0
package-retroarch-macos:
	@test "$$(uname -s)" = Darwin || { printf '%s\n' 'This target requires macOS and Xcode Command Line Tools.' >&2; exit 1; }
	$(MAKE) -C $(src_dir)/libretro build build_dir="$(MACOS_PACKAGE_BUILD_DIR)" CXX=clang++ CPPFLAGS="$(MACOS_PACKAGE_FLAGS)"
	python3 tools/package_retroarch_macos.py --core "$(MACOS_PACKAGE_BUILD_DIR)/lotusgb_libretro.dylib" \
		--info "$(MACOS_PACKAGE_BUILD_DIR)/lotusgb_libretro.info" --output-dir "$(MACOS_PACKAGE_DIR)"
	python3 tools/test_macos_package.py --package "$(MACOS_PACKAGE_DIR)/LotusGB-RetroArch-macOS-universal.zip"

STEAM_DECK_PACKAGE_BUILD_DIR ?= $(CURDIR)/build/steamdeck-package
STEAM_DECK_PACKAGE_DIR ?= $(CURDIR)/build/packages
STEAM_DECK_ZIG ?= $(CURDIR)/build/toolchains/zig-0.17.0/zig
STEAM_DECK_TARGET ?= x86_64-linux-gnu
STEAM_DECK_PACKAGE_CXXFLAGS ?= -g0 -Wl,--strip-debug
package-retroarch-steam-deck:
	@test -x "$(STEAM_DECK_ZIG)" || { printf '%s\n' 'Zig 0.17.0 not found; set STEAM_DECK_ZIG.' >&2; exit 1; }
	ZIG_GLOBAL_CACHE_DIR="$(CURDIR)/build/zig-cache/global" \
	ZIG_LOCAL_CACHE_DIR="$(CURDIR)/build/zig-cache/local" \
	$(MAKE) -C $(src_dir)/libretro build_dir="$(STEAM_DECK_PACKAGE_BUILD_DIR)" \
		CXX="$(STEAM_DECK_ZIG) c++ -target $(STEAM_DECK_TARGET) $(STEAM_DECK_PACKAGE_CXXFLAGS)" \
		"$(STEAM_DECK_PACKAGE_BUILD_DIR)/lotusgb_libretro.so" \
		"$(STEAM_DECK_PACKAGE_BUILD_DIR)/lotusgb_libretro.info"
	python3 tools/package_retroarch_linux.py --platform steam-deck \
		--core "$(STEAM_DECK_PACKAGE_BUILD_DIR)/lotusgb_libretro.so" \
		--info "$(STEAM_DECK_PACKAGE_BUILD_DIR)/lotusgb_libretro.info" \
		--output-dir "$(STEAM_DECK_PACKAGE_DIR)"
