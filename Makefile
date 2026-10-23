# Let src/Makefile handle all builds, pass some variables

src_dir := $(CURDIR)/src
build_dir := $(CURDIR)/build

# PHONY: non file targets
.PHONY: build test test-roms test-mooneye clean

TEST_ROM_DIR ?= ../test-roms/dmg_sound
TEST_ROM_FRAMES ?= 1200
MOONEYE_SUITE_DIR ?= ../test-roms/mooneye

all: build

$(build_dir):
	mkdir -p $@

build: $(build_dir)
	$(MAKE) -C $(src_dir) build_dir=$(build_dir)

test: $(build_dir)
	$(MAKE) -C $(src_dir) test build_dir=$(build_dir)

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
		--output-dir "$(build_dir)/test-roms/mooneye"

clean:
	rm $(build_dir) -r
