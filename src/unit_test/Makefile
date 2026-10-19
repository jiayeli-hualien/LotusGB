CXX ?= g++
CPPFLAGS := -Wall -Wextra -std=c++17 -O2 -I../lib/include
build_dir ?= build

test_source := cpu_cycle_test.cpp
test_headers := $(wildcard ../lib/include/*.hpp)
core_sources := ../lib/GBCPU.cpp ../lib/decoder.cpp ../lib/instruction.cpp \
	../lib/instSubFunc.cpp ../lib/instSubFuncCB.cpp ../lib/instSubFuncCommon.cpp
generated_tables := ../lib/include/generated_decode_tables.hpp
csv_sources := $(wildcard ../lib/include/*.csv)
test_executables := $(build_dir)/cpu_interface_test $(build_dir)/cpu_instruction_test \
	$(build_dir)/adapter_lifetime_test

.PHONY: test

test: $(test_executables)
	$(build_dir)/cpu_interface_test
	$(build_dir)/cpu_instruction_test
	$(build_dir)/adapter_lifetime_test

$(build_dir)/cpu_interface_test: cpu_cycle_test.cpp ../lib/GBCPU.cpp $(test_headers) | $(build_dir)
	$(CXX) $(CPPFLAGS) $< ../lib/GBCPU.cpp -o $@

$(generated_tables): $(csv_sources) ../../tools/generate_decode_tables.py
	python3 ../../tools/generate_decode_tables.py

$(build_dir)/cpu_instruction_test: cpu_instruction_test.cpp $(core_sources) $(test_headers) $(generated_tables) | $(build_dir)
	$(CXX) $(CPPFLAGS) $< $(core_sources) -o $@

$(build_dir)/adapter_lifetime_test: adapter_lifetime_test.cpp $(wildcard ../lib/*.cpp) $(test_headers) $(generated_tables) | $(build_dir)
	$(CXX) $(CPPFLAGS) $< $(wildcard ../lib/*.cpp) -o $@

$(build_dir):
	mkdir -p $@
