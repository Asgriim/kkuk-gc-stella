#include "support.hpp"

using namespace gc_test;

namespace {

void updateBarrier() {
    Root owner;
    Root child;

    owner.allocate(cleared_descriptor);
    untilMinor();

    child.allocate(leaf_descriptor);
    stella_object_init_primitive(child.get(), 0, 908);

    std::array<StellaValue *, 1> values{child.slot()};
    const auto before = statistics().at("write barrier activations");
    stella_gc_update_object(owner.slot(), &closure_descriptor, values.data(), nullptr);

    check(
        statistics().at("write barrier activations") == before + 1, "update omitted write barrier");
    child.set(nullptr);
    untilMinor();

    child.set(stella_gc_read_managed(owner.slot(), 0));
    check(stella_object_read_primitive(child.get(), 0) == 908, "new layout edge lost");

    stella_gc_update_object(owner.slot(), &cleared_descriptor, nullptr, nullptr);
    child.set(nullptr);
    churn();

    checkLayout(owner.get());
}

}

int main(int argc, char **argv) {
    return runScenario(argc, argv, updateBarrier);
}
