#include "support.hpp"

using namespace gc_test;

namespace {

void largeInit() {
    Root child;
    Root large;

    child.allocate(leaf_descriptor);
    stella_object_init_primitive(child.get(), 0, 876);

    const auto before = statistics().at("write barrier activations");
    large.allocate(large_descriptor);
    stella_object_init_managed(large.get(), 0, child.get());
    stella_object_init_primitive(large.get(), 0, 19);

    check(statistics().at("write barrier activations") == before, "initialization invoked barrier");
    child.set(nullptr);
    untilMinor();

    child.set(stella_gc_read_managed(large.slot(), 0));
    check(stella_object_read_primitive(child.get(), 0) == 876,
        "large initialization lost young edge");
    checkLayout(large.get());
    large.set(nullptr);
    child.set(nullptr);
    const auto full_before = statistics().at("full collections");

    for (std::size_t i = 0; i < 100; ++i) {
        large.allocate(large_descriptor);
        stella_object_init_managed(large.get(), 0, nullptr);
        stella_object_init_primitive(large.get(), 0, i);
        large.set(nullptr);
    }

    check(
        statistics().at("full collections") > full_before, "dead remembered objects not reclaimed");
}

}

int main(int argc, char **argv) {
    return runScenario(argc, argv, largeInit);
}
