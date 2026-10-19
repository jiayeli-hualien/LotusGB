CC := g++
CPPFLAGS := -Wall -fPIC --std=c++17 -O2
LDFLAGS := -shared
lotus_gb_lib_build_path := $(build_dir)/liblotusgb.so
obj_build_dir := $(build_dir)/obj

SRCS := $(wildcard *.cpp)
OBJS := $(patsubst %.cpp,$(obj_build_dir)/%.o,$(SRCS))

include_path = $(CURDIR)/include
generated_tables := $(include_path)/generated_decode_tables.hpp
csv_sources := $(wildcard $(include_path)/*.csv)

all: $(lotus_gb_lib_build_path)

$(obj_build_dir) $(build_dir):
	mkdir -p $@

$(generated_tables): $(csv_sources) $(CURDIR)/../../tools/generate_decode_tables.py
	python3 $(CURDIR)/../../tools/generate_decode_tables.py

$(obj_build_dir)/%.o: %.cpp $(obj_build_dir) $(generated_tables)
	$(CC) $(CPPFLAGS) -c $< -o $@

$(lotus_gb_lib_build_path): $(OBJS)
	$(CC) $(CPPFLAGS) -o $@ $^ $(LDFLAGS)
