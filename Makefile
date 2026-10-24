# Let src/Makefile handle all builds, pass some variables

src_dir := $(CURDIR)/src
build_dir := $(CURDIR)/build

# PHONY: non file targets
.PHONY: build test test-roms test-mooneye test-homebrew clean

TEST_ROM_DIR ?= ../test-roms/dmg_sound
TEST_ROM_FRAMES ?= 1200
HOMEBREW_GAME ?= big2small
HOMEBREW_ROM ?= ../test-roms/homebrew/$(if $(filter tobutobugirl,$(HOMEBREW_GAME)),tobutobugirl/tobu.gb,big2small.gb)
HOMEBREW_SAVE ?=
MOONEYE_GROUP ?= ppu
MOONEYE_OUTPUT_DIR ?= $(build_dir)/test-roms/mooneye$(if $(filter ppu,$(MOONEYE_GROUP)),,-$(MOONEYE_GROUP))
MOONEYE_SUITE_DIR ?= ../test-roms/mooneye

all: build

$(build_dir):
	mkdir -p $@

build: $(build_dir)
	$(MAKE) -C $(src_dir) build_dir=$(build_dir)

test: $(build_dir)
	$(MAKE) -C $(src_dir) test build_dir=$(build_dir)
	$(MAKE) -C $(src_dir)/lib mooneye-runner build_dir=$(build_dir)
	python3 tools/test_mooneye_runner.py --runner "$(build_dir)/mooneye_rom_runner"
	python3 tools/test_homebrew_check.py

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

test-homebrew:
	$(MAKE) -C $(src_dir)/libretro build rom-test-runner build_dir=$(build_dir)
	python3 tools/run_homebrew_check.py \
		--game "$(HOMEBREW_GAME)" \
		--core "$(build_dir)/lotusgb_libretro.so" \
		--runner "$(build_dir)/libretro_rom_runner" \
		--rom "$(HOMEBREW_ROM)" \
		$(if $(HOMEBREW_SAVE),--save "$(HOMEBREW_SAVE)")

clean:
	rm $(build_dir) -r
