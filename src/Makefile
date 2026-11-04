# Let three sub folders build, pass arguments

executables := gbit_test libretro
sub_folders := lib $(executables)

.PHONY: $(sub_folders) gba_libretro gba_libretro_test unit_test libretro_test test

all: $(sub_folders) gba_libretro

$(executables): lib
	$(MAKE) -C ./$@ build_dir=$(build_dir)

gba_libretro:
	$(MAKE) -C ./gba_libretro build_dir=$(build_dir)

unit_test:
	$(MAKE) -C ./unit_test build_dir=$(build_dir)

libretro_test:
	$(MAKE) -C ./libretro test build_dir=$(build_dir)

gba_libretro_test:
	$(MAKE) -C ./gba_libretro test build_dir=$(build_dir)

test: unit_test libretro_test gba_libretro_test

# TODO: re-write makefile to leverage dependycy & parallel build
lib:
	$(MAKE) -C ./lib build_dir=$(build_dir)
