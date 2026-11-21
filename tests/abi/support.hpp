#pragma once

#include "../../internal/stella_abi.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <source_location>
#include <string>
#include <string_view>

namespace gc_test {

void check(bool condition,
    std::string_view message,
    std::source_location location = std::source_location::current());

class Root {
  public:
    explicit Root(StellaValue value = nullptr) : value_(value) {
        stella_gc_push_root(&value_);
    }

    ~Root() {
        stella_gc_pop_root(&value_);
    }

    Root(const Root &) = delete;
    Root &operator=(const Root &) = delete;

    StellaValue get() const {
        return value_;
    }

    StellaValue *slot() {
        return &value_;
    }

    void set(StellaValue value) {
        value_ = value;
    }

    void allocate(const StellaObjectDescriptor &descriptor) {
        value_ = stella_gc_alloc(&descriptor);
    }

  private:
    StellaValue value_;
};

inline void entry(StellaValue *, StellaValue *, std::size_t, StellaValue *const *) {
}

constexpr StellaObjectDescriptor descriptor(std::size_t managed,
    std::size_t primitive,
    std::size_t capacity,
    std::uint32_t flags = 0,
    std::uint32_t kind = STELLA_OBJECT_DATA) {
    return {.abi_version = STELLA_C_GC_ABI_VERSION,
        .kind = kind,
        .flags = flags,
        .entry_arity = 0,
        .managed_count = managed,
        .primitive_count = primitive,
        .slot_capacity = capacity,
        .entry_code =
            kind == STELLA_OBJECT_CLOSURE || kind == STELLA_OBJECT_THUNK ? entry : nullptr,
        .debug_name = "test-object"};
}

inline constexpr auto empty_descriptor = descriptor(0, 0, 0);
inline constexpr auto leaf_descriptor = descriptor(0, 1, 1);
inline constexpr auto node_descriptor = descriptor(1, 1, 2);
inline constexpr auto pair_descriptor = descriptor(2, 1, 4, STELLA_DESCRIPTOR_UPDATABLE);
inline constexpr auto mixed_descriptor = descriptor(2, 2, 6);

inline constexpr auto changed_descriptor = descriptor(1, 2, 4, STELLA_DESCRIPTOR_UPDATABLE);
inline constexpr auto cleared_descriptor = descriptor(0, 0, 4, STELLA_DESCRIPTOR_UPDATABLE);

inline constexpr auto closure_descriptor =
    descriptor(1, 0, 4, STELLA_DESCRIPTOR_UPDATABLE, STELLA_OBJECT_CLOSURE);
inline constexpr auto thunk_descriptor =
    descriptor(1, 0, 4, STELLA_DESCRIPTOR_UPDATABLE, STELLA_OBJECT_THUNK);
inline constexpr auto indirection_descriptor = descriptor(1, 0, 4, 0, STELLA_OBJECT_INDIRECTION);

inline constexpr auto large_descriptor = descriptor(1, 1, 32);

inline constexpr auto static_leaf_descriptor = descriptor(0, 0, 0, STELLA_DESCRIPTOR_STATIC);
inline constexpr auto static_descriptor = descriptor(1, 1, 2, STELLA_DESCRIPTOR_STATIC);
inline constexpr auto static_update_descriptor = descriptor(0, 0, 4, STELLA_DESCRIPTOR_STATIC);

extern StellaObject static_leaf;
extern StellaObject static_object;
extern std::size_t heap_limit;

using Statistics = std::map<std::string, std::size_t>;

std::size_t objectBytes(const StellaObjectDescriptor &value);

std::string capture(void (*print)(FILE *));
Statistics statistics();
void checkLayout(StellaValue object);

void churn(std::size_t count = 400);
void untilMinor();

void configureHeap(int argc, char **argv);
int runScenario(int argc, char **argv, void (*scenario)());

}
