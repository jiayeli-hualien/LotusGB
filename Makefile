# Let src/Makefile handle all builds, pass some variables

src_dir := $(CURDIR)/src
build_dir := $(CURDIR)/build

# PHONY: non file targets
.PHONY: build test test-roms test-mooneye test-mooneye-visual test-homebrew \
	test-gbmicrotest-ppu-latch install-retroarch-core-macos clean

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
MOONEYE_VISUAL_SUITE_DIR ?= ../test-roms/game-boy-test-roms-v7.0/mooneye-test-suite
VISUAL_TEST_PYTHON ?= $(build_dir)/visual-venv/bin/python
GBMICROTEST_SUITE_DIR ?= ../test-roms/gbmicrotest
RETROARCH_CORE_DIR ?= $(HOME)/Library/Application Support/RetroArch/cores

all: build

$(build_dir):
	mkdir -p $@

build: $(build_dir)
	$(MAKE) -C $(src_dir) build_dir=$(build_dir)

install-retroarch-core-macos: $(build_dir)
	@test "$$(uname -s)" = Darwin || { printf '%s\n' 'This target requires macOS.' >&2; exit 1; }
	$(MAKE) -C $(src_dir)/libretro build build_dir=$(build_dir)
	@test -d "$(RETROARCH_CORE_DIR)" || { printf '%s\n' 'RetroArch core directory not found; set RETROARCH_CORE_DIR.' >&2; exit 1; }
	@set -eu; \
		tmp="$$(mktemp "$(RETROARCH_CORE_DIR)/.lotusgb_libretro.XXXXXXXX")"; \
		trap 'rm -f "$$tmp"' EXIT; \
		cp "$(build_dir)/lotusgb_libretro.dylib" "$$tmp"; \
		chmod 755 "$$tmp"; \
		mv -f "$$tmp" "$(RETROARCH_CORE_DIR)/lotusgb_libretro.dylib"
	@printf '%s\n' 'Installed LotusGB core. Restart RetroArch to load it.'

test: $(build_dir)
	$(MAKE) -C $(src_dir) test build_dir=$(build_dir)
	$(MAKE) -C $(src_dir)/lib mooneye-runner build_dir=$(build_dir)
	python3 tools/test_mooneye_runner.py --runner "$(build_dir)/mooneye_rom_runner"
	python3 tools/test_homebrew_check.py
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
		--output-dir "$(MOONEYE_OUTPUT_DIR)"

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

test-gbmicrotest-ppu-latch:
	$(MAKE) -C $(src_dir)/libretro build rom-test-runner build_dir=$(build_dir)
	python3 tools/run_gbmicrotest_ppu_latches.py \
		--suite-dir "$(GBMICROTEST_SUITE_DIR)" \
		--core "$(build_dir)/lotusgb_libretro.so" \
		--runner "$(build_dir)/libretro_rom_runner"

clean:
	rm $(build_dir) -r
