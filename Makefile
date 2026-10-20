CXX ?= g++
CPPFLAGS ?= -Iinclude
CXXFLAGS ?= -O2 -std=c++20 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=

COMMON_SOURCES := $(filter-out src/main.cpp,$(wildcard src/*.cpp))
COMMON_OBJECTS := $(patsubst %.cpp,build/%.o,$(COMMON_SOURCES))
TEST_OBJECTS := $(patsubst %.cpp,build/%.o,$(wildcard tests/*.cpp))
ALL_OBJECTS := $(COMMON_OBJECTS) $(TEST_OBJECTS) build/src/main.o

.PHONY: all check clean
all: build/vm-fork-bench build/vm-fork-bench-tests

build/vm-fork-bench: $(COMMON_OBJECTS) build/src/main.o
	$(CXX) $(LDFLAGS) $^ -o $@

build/vm-fork-bench-tests: $(COMMON_OBJECTS) $(TEST_OBJECTS)
	$(CXX) $(LDFLAGS) $^ -o $@

build/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

check: all
	./build/vm-fork-bench-tests

clean:
	rm -rf build

-include $(ALL_OBJECTS:.o=.d)
