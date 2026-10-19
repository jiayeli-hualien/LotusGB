CXX ?= g++
CPPFLAGS := -Wall -Wextra -std=c++17 -O2 -I../lib/include

test_source := cpu_cycle_test.cpp
test_headers := $(wildcard ../lib/include/*.hpp)
test_executable := $(build_dir)/cpu_interface_test

.PHONY: test

test: $(test_executable)
	$(test_executable)

$(test_executable): $(test_source) ../lib/GBCPU.cpp $(test_headers) | $(build_dir)
	$(CXX) $(CPPFLAGS) $(test_source) ../lib/GBCPU.cpp -o $@

$(build_dir):
	mkdir -p $@
