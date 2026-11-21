#include "support.hpp"

#include <limits>

using namespace gc_test;

namespace {

StellaValue permanent_value = nullptr;

void graph() {
    Root shared;
    Root first;
    Root second;
    Root alias;
    Root static_root(&static_object);
    Root null_root;

    shared.allocate(leaf_descriptor);
    stella_object_init_primitive(shared.get(), 0, 98765);

    first.allocate(pair_descriptor);
    stella_object_init_managed(first.get(), 0, nullptr);
    stella_object_init_managed(first.get(), 1, shared.get());
    const auto pointer_bits = reinterpret_cast<StellaPrimitive>(shared.get());
    stella_object_init_primitive(first.get(), 0, pointer_bits);

    second.allocate(pair_descriptor);
    stella_object_init_managed(second.get(), 0, first.get());
    stella_object_init_managed(second.get(), 1, shared.get());
    stella_object_init_primitive(second.get(), 0, std::numeric_limits<StellaPrimitive>::max());

    stella_gc_write_managed(first.slot(), 0, second.slot());
    alias.set(first.get());
    permanent_value = first.get();
    stella_gc_register_permanent_root(&permanent_value);
    shared.set(nullptr);
    second.set(nullptr);
    churn();

    check(first.get() == alias.get() && first.get() == permanent_value, "root aliases diverged");

    second.set(stella_gc_read_managed(first.slot(), 0));
    check(stella_gc_read_managed(second.slot(), 0) == first.get(), "cycle was broken");

    shared.set(stella_gc_read_managed(first.slot(), 1));
    check(stella_gc_read_managed(second.slot(), 1) == shared.get(), "shared child was duplicated");
    check(stella_object_read_primitive(shared.get(), 0) == 98765, "shared child was corrupted");
    check(
        stella_object_read_primitive(first.get(), 0) == pointer_bits, "primitive pointer changed");
    check(stella_object_read_primitive(second.get(), 0) ==
              std::numeric_limits<StellaPrimitive>::max(),
        "primitive word changed");

    check(static_root.get() == &static_object, "static object moved");
    check(stella_gc_read_managed(static_root.slot(), 0) == &static_leaf, "static field changed");
    check(stella_object_read_primitive(static_root.get(), 0) == 12345, "static primitive changed");
    check(static_object.gc_word == 29 && static_leaf.gc_word == 17, "static metadata changed");

    check(null_root.get() == nullptr, "null root changed");

    checkLayout(first.get());
    checkLayout(second.get());

    check(statistics().at("collections") > 0, "graph was not tested across a collection");
}

}

int main(int argc, char **argv) {
    return runScenario(argc, argv, graph);
}
