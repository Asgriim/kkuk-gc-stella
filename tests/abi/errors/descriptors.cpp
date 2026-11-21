#include "support.hpp"

#include <limits>

namespace gc_test {

void runErrorScenario(std::string_view name) {
    initializeCollector();

    if (name == "null") {
        static_cast<void>(stella_gc_alloc(nullptr));
        return;
    }

    auto invalid = empty_descriptor;

    if (name == "version") {
        invalid.abi_version = 2;
    } else if (name == "static") {
        invalid.flags = STELLA_DESCRIPTOR_STATIC;
    } else if (name == "capacity") {
        invalid.managed_count = 1;
    } else if (name == "count_overflow") {
        invalid.managed_count = std::numeric_limits<std::size_t>::max();
        invalid.primitive_count = 1;
        invalid.slot_capacity = std::numeric_limits<std::size_t>::max();
    } else if (name == "multiply_overflow") {
        invalid.slot_capacity = std::numeric_limits<std::size_t>::max() / sizeof(StellaSlot) + 1;
    } else if (name == "header_overflow") {
        invalid.slot_capacity =
            (std::numeric_limits<std::size_t>::max() - sizeof(StellaObject)) / sizeof(StellaSlot) +
            1;
    } else {
        check(false, "unknown descriptor error");
    }

    static_cast<void>(stella_gc_alloc(&invalid));
}

}
