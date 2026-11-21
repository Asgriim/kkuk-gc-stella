#include "support.hpp"

#include <limits>

using namespace gc_test;

namespace {

void layouts() {
    Root empty;
    Root mixed;

    empty.allocate(empty_descriptor);
    mixed.allocate(mixed_descriptor);

    check(mixed.get()->fields[0].managed == nullptr, "managed field not initially null");
    check(mixed.get()->fields[1].managed == nullptr, "managed field not initially null");

    for (std::size_t i = 2; i < 6; ++i) {
        check(mixed.get()->fields[i].primitive == 0, "slot not initially zero");
    }

    stella_object_init_managed(mixed.get(), 0, empty.get());
    stella_object_init_managed(mixed.get(), 1, &static_leaf);
    stella_object_init_primitive(mixed.get(), 0, 42);
    stella_object_init_primitive(mixed.get(), 1, std::numeric_limits<StellaPrimitive>::max());

    stella_object_write_primitive(mixed.get(), 0, 73);

    churn();

    check(stella_gc_read_managed(mixed.slot(), 0) == empty.get(), "zero-slot object lost");
    check(stella_gc_read_managed(mixed.slot(), 1) == &static_leaf, "managed field overwritten");
    check(stella_object_read_primitive(mixed.get(), 0) == 73, "primitive offset is wrong");
    check(
        stella_object_read_primitive(mixed.get(), 1) == std::numeric_limits<StellaPrimitive>::max(),
        "primitive maximum lost");
    checkLayout(empty.get());
    checkLayout(mixed.get());
}

}

int main(int argc, char **argv) {
    return runScenario(argc, argv, layouts);
}
