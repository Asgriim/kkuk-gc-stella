CC = cc
CXX = c++
AR = ar
PYTHON ?= python3
DOCKER ?= docker
CLANG_FORMAT ?= clang-format

CPPFLAGS ?=
CFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic
CXXFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?=

EXAMPLES := double playlist chat history
PROGRAMS := $(addprefix build/,$(EXAMPLES))
RUNTIME_OBJECTS := build/runtime/runtime.o build/runtime/scalar.o build/runtime/main.o
EXAMPLE_OBJECTS := $(addprefix build/examples/,$(addsuffix .o,$(EXAMPLES)))
GC_OBJECT := build/gc.o
GC_LIBRARY := build/libstella_gc.a
TEST_SOURCES := $(filter-out tests/abi/support.cpp,$(wildcard tests/abi/*.cpp))
TEST_SCENARIOS := $(basename $(notdir $(TEST_SOURCES)))
TEST_CPP_PROGRAMS := $(addprefix build/gc_test_,$(TEST_SCENARIOS))
ERROR_TEST_SOURCES := $(filter-out tests/abi/errors/support.cpp,$(wildcard tests/abi/errors/*.cpp))
ERROR_TEST_SCENARIOS := $(basename $(notdir $(ERROR_TEST_SOURCES)))
ERROR_TEST_PROGRAMS := $(addprefix build/gc_test_error_,$(ERROR_TEST_SCENARIOS))
ERROR_TEST_OBJECTS := $(addprefix build/,$(ERROR_TEST_SOURCES:.cpp=.o)) build/tests/abi/errors/support.o
TEST_PROGRAMS := $(TEST_CPP_PROGRAMS) $(ERROR_TEST_PROGRAMS) build/gc_test_c_abi_smoke
TEST_OBJECTS := $(addprefix build/,$(TEST_SOURCES:.cpp=.o)) \
    build/tests/abi/support.o build/tests/abi/c_abi_smoke.o
STELLA_TEST_SOURCES := $(wildcard tests/stella/src/*.stella)
STELLA_TEST_PROGRAMS := $(addprefix build/stella_test_,$(basename $(notdir $(STELLA_TEST_SOURCES))))
STELLA_TEST_OBJECTS := $(addprefix build/tests/stella/,$(addsuffix .o,$(basename $(notdir $(STELLA_TEST_SOURCES)))))
OBJECTS := $(RUNTIME_OBJECTS) $(EXAMPLE_OBJECTS) $(GC_OBJECT) $(TEST_OBJECTS) $(ERROR_TEST_OBJECTS) $(STELLA_TEST_OBJECTS)
FORMAT_SOURCES := gc.cpp $(wildcard internal/*.h internal/*.hpp tests/abi/*.cpp tests/abi/*.hpp tests/abi/*.c tests/abi/errors/*.cpp tests/abi/errors/*.hpp)

.PHONY: all clean format format-check test test-stella
.SECONDARY: $(OBJECTS) $(STELLA_TEST_OBJECTS:.o=.c)

all: $(PROGRAMS)

test: $(TEST_PROGRAMS)
	$(PYTHON) tests/run_tests.py --bin-dir build

test-stella: $(STELLA_TEST_PROGRAMS)
	$(PYTHON) tests/run_tests.py --bin-dir build --suite stella

$(TEST_CPP_PROGRAMS): build/gc_test_%: build/tests/abi/%.o build/tests/abi/support.o build/runtime/runtime.o $(GC_LIBRARY)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(ERROR_TEST_PROGRAMS): build/gc_test_error_%: build/tests/abi/errors/%.o build/tests/abi/errors/support.o build/tests/abi/support.o build/runtime/runtime.o $(GC_LIBRARY)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/gc_test_c_abi_smoke: build/tests/abi/c_abi_smoke.o build/runtime/runtime.o $(GC_LIBRARY)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(STELLA_TEST_PROGRAMS): build/stella_test_%: build/tests/stella/%.o $(RUNTIME_OBJECTS) $(GC_LIBRARY)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/tests/stella/%.o: build/tests/stella/%.c
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) -std=c11 -MMD -MP -c $< -o $@

build/tests/stella/%.c: tests/stella/src/%.stella tests/stella/compile.py
	$(PYTHON) tests/stella/compile.py --docker "$(DOCKER)" $< $@

format:
	$(CLANG_FORMAT) -i $(FORMAT_SOURCES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_SOURCES)

$(PROGRAMS): build/%: build/examples/%.o $(RUNTIME_OBJECTS) $(GC_LIBRARY)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(GC_LIBRARY): $(GC_OBJECT)
	$(AR) rcs $@ $^

$(GC_OBJECT): gc.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) -Iinclude $(CXXFLAGS) -std=c++20 -MMD -MP -c $< -o $@

build/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) -std=c11 -MMD -MP -c $< -o $@

build/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) -Iinclude $(CXXFLAGS) -std=c++20 -MMD -MP -c $< -o $@

clean:
	rm -rf build

-include $(OBJECTS:.o=.d)
