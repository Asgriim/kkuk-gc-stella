CC = cc
CXX = c++
AR = ar

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
OBJECTS := $(RUNTIME_OBJECTS) $(EXAMPLE_OBJECTS) $(GC_OBJECT)

.PHONY: all clean
.SECONDARY: $(OBJECTS)

all: $(PROGRAMS)

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

clean:
	rm -rf build

-include $(OBJECTS:.o=.d)
