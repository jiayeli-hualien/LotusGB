CXX ?= g++
CPPFLAGS := -Wall -Wextra -std=c++17 -O2 -I../lib/include
build_dir ?= build

test_source := cpu_cycle_test.cpp
test_headers := $(wildcard ../lib/include/*.hpp)
core_sources := ../lib/GBCPU.cpp ../lib/decoder.cpp ../lib/instruction.cpp \
	../lib/instSubFunc.cpp ../lib/instSubFuncCB.cpp ../lib/instSubFuncCommon.cpp \
	../lib/interrupt_registers.cpp
generated_tables := ../lib/include/generated_decode_tables.hpp
csv_sources := $(wildcard ../lib/include/*.csv)
test_executables := $(build_dir)/save_state_test  $(build_dir)/cpu_interface_test $(build_dir)/cpu_instruction_test \
	$(build_dir)/adapter_lifetime_test $(build_dir)/system_clock_test \
	$(build_dir)/address_bus_test $(build_dir)/game_boy_system_test \
	$(build_dir)/timer_test $(build_dir)/dmg_serial_test \
	$(build_dir)/joypad_test $(build_dir)/dmg_ppu_test \
	$(build_dir)/dmg_oam_dma_test \
	$(build_dir)/dmg_apu_test \
	$(build_dir)/dmg_acid2_test

acid2_asset_dir := ../../tests/roms/dmg-acid2
acid2_assets := $(wildcard ../../tests/roms/dmg-acid2/*)

.PHONY: test

test: $(test_executables)
	$(build_dir)/save_state_test
	$(build_dir)/cpu_interface_test
	$(build_dir)/cpu_instruction_test
	$(build_dir)/adapter_lifetime_test
	$(build_dir)/system_clock_test
	$(build_dir)/address_bus_test
	$(build_dir)/game_boy_system_test
	$(build_dir)/timer_test
	$(build_dir)/dmg_serial_test
	$(build_dir)/joypad_test
	$(build_dir)/dmg_ppu_test
	$(build_dir)/dmg_oam_dma_test
	$(build_dir)/dmg_apu_test
	$(build_dir)/dmg_acid2_test

$(build_dir)/cpu_interface_test: cpu_cycle_test.cpp ../lib/GBCPU.cpp $(test_headers) | $(build_dir)
	$(CXX) $(CPPFLAGS) $< ../lib/GBCPU.cpp -o $@

$(generated_tables): $(csv_sources) ../../tools/generate_decode_tables.py
	python3 ../../tools/generate_decode_tables.py

$(build_dir)/cpu_instruction_test: cpu_instruction_test.cpp $(core_sources) $(test_headers) $(generated_tables) | $(build_dir)
	$(CXX) $(CPPFLAGS) $< $(core_sources) -o $@

$(build_dir)/adapter_lifetime_test: adapter_lifetime_test.cpp $(wildcard ../lib/*.cpp) $(test_headers) $(generated_tables) | $(build_dir)
	$(CXX) $(CPPFLAGS) $< $(wildcard ../lib/*.cpp) -o $@

$(build_dir)/system_clock_test: system_clock_test.cpp ../lib/system_clock.cpp $(test_headers) | $(build_dir)
	$(CXX) $(CPPFLAGS) $< ../lib/system_clock.cpp -o $@

$(build_dir)/address_bus_test: address_bus_test.cpp ../lib/address_bus.cpp \
		../lib/cartridge.cpp ../lib/memory_region.cpp $(test_headers) | $(build_dir)
	$(CXX) $(CPPFLAGS) $< ../lib/address_bus.cpp ../lib/cartridge.cpp \
		../lib/memory_region.cpp -o $@

$(build_dir)/game_boy_system_test: game_boy_system_test.cpp $(wildcard ../lib/*.cpp) $(test_headers) $(generated_tables) | $(build_dir)
	$(CXX) $(CPPFLAGS) $< $(wildcard ../lib/*.cpp) -o $@

$(build_dir)/timer_test: timer_test.cpp ../lib/timer.cpp \
		../lib/interrupt_registers.cpp $(test_headers) | $(build_dir)
	$(CXX) $(CPPFLAGS) $< ../lib/timer.cpp ../lib/interrupt_registers.cpp -o $@

$(build_dir)/dmg_serial_test: dmg_serial_test.cpp ../lib/dmg_serial.cpp \
		../lib/interrupt_registers.cpp $(test_headers) | $(build_dir)
	$(CXX) $(CPPFLAGS) $< ../lib/dmg_serial.cpp ../lib/interrupt_registers.cpp -o $@

$(build_dir)/joypad_test: joypad_test.cpp ../lib/joypad.cpp \
		../lib/interrupt_registers.cpp $(test_headers) | $(build_dir)
	$(CXX) $(CPPFLAGS) $< ../lib/joypad.cpp ../lib/interrupt_registers.cpp -o $@

$(build_dir)/dmg_ppu_test: dmg_ppu_test.cpp ../lib/dmg_ppu.cpp \
		../lib/interrupt_registers.cpp $(test_headers) | $(build_dir)
	$(CXX) $(CPPFLAGS) $< ../lib/dmg_ppu.cpp \
		../lib/interrupt_registers.cpp -o $@

$(build_dir)/dmg_oam_dma_test: dmg_oam_dma_test.cpp ../lib/dmg_oam_dma.cpp \
		../lib/dmg_ppu.cpp ../lib/address_bus.cpp ../lib/interrupt_registers.cpp \
		../lib/memory_region.cpp $(test_headers) | $(build_dir)
	$(CXX) $(CPPFLAGS) $< ../lib/dmg_oam_dma.cpp ../lib/dmg_ppu.cpp \
		../lib/address_bus.cpp ../lib/interrupt_registers.cpp \
		../lib/memory_region.cpp -o $@

$(build_dir)/dmg_apu_test: dmg_apu_test.cpp ../lib/dmg_apu.cpp $(test_headers) | $(build_dir)
	$(CXX) $(CPPFLAGS) $< ../lib/dmg_apu.cpp -o $@

$(build_dir)/dmg_acid2_test: dmg_acid2_test.cpp $(wildcard ../lib/*.cpp) \
		$(test_headers) $(generated_tables) $(acid2_assets) | $(build_dir)
	$(CXX) $(CPPFLAGS) -DLOTUSGB_ACID2_ASSET_DIR=\"$(acid2_asset_dir)\" \
		$< $(wildcard ../lib/*.cpp) -o $@

$(build_dir):
	mkdir -p $@

$(build_dir)/save_state_test: save_state_test.cpp $(wildcard ../lib/*.cpp) $(test_headers) $(generated_tables) | $(build_dir)
	$(CXX) $(CPPFLAGS) $< $(wildcard ../lib/*.cpp) -o $@
