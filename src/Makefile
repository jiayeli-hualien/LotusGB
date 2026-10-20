# Let three sub folders build, pass arguments

executables := application gbit_test libretro
sub_folders := lib $(executables)

.PHONY: $(sub_folders) unit_test libretro_test test

all: $(sub_folders)

$(executables): lib
	$(MAKE) -C ./$@ build_dir=$(build_dir)

unit_test:
	$(MAKE) -C ./unit_test build_dir=$(build_dir)

libretro_test:
	$(MAKE) -C ./libretro test build_dir=$(build_dir)

test: unit_test libretro_test

# TODO: re-write makefile to leverage dependycy & parallel build
lib:
	$(MAKE) -C ./lib build_dir=$(build_dir)
