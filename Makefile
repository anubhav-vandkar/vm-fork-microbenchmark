CXX ?= g++
CPPFLAGS ?= -I.
CXXFLAGS ?= -O2 -std=c++20 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=

COMMON_SOURCES := benchmark.cpp vm.cpp transport.cpp metrics.cpp report.cpp
COMMON_OBJECTS := $(patsubst %.cpp,build/%.o,$(COMMON_SOURCES))
TEST_OBJECTS := build/tests/test.o build/cli-test.o
ALL_OBJECTS := $(COMMON_OBJECTS) $(TEST_OBJECTS) build/main.o

.PHONY: all check clean
all: build/vm-fork-bench build/vm-fork-bench-tests

build/vm-fork-bench: $(COMMON_OBJECTS) build/main.o
	$(CXX) $(LDFLAGS) $^ -o $@

build/vm-fork-bench-tests: $(COMMON_OBJECTS) $(TEST_OBJECTS)
	$(CXX) $(LDFLAGS) $^ -o $@

build/cli-test.o: main.cpp benchmark.hpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DVMBENCH_TESTING -MMD -MP -c $< -o $@

build/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

check: all
	./build/vm-fork-bench-tests

clean:
	rm -rf build

-include $(ALL_OBJECTS:.o=.d)
