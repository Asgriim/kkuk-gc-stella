#include "support.hpp"

#include <vector>

using namespace gc_test;

namespace {

StellaValue permanent_value = nullptr;

void roots() {
    Root value;

    value.allocate(leaf_descriptor);
    stella_object_init_primitive(value.get(), 0, 31415);

    std::vector<StellaValue> slots(4096, nullptr);

    for (auto &slot : slots) {
        stella_gc_push_root(&slot);
    }

    slots.front() = value.get();
    slots.back() = value.get();

    churn();

    check(slots.front() == value.get() && slots.back() == value.get(), "deep roots lost aliases");
    check(statistics().at("maximum dynamic roots") >= slots.size(), "root depth truncated");

    for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
        stella_gc_pop_root(&*it);
    }

    permanent_value = nullptr;
    stella_gc_register_permanent_root(&permanent_value);
    permanent_value = value.get();
    value.set(nullptr);
    churn();

    check(stella_object_read_primitive(permanent_value, 0) == 31415, "permanent-only object lost");
    value.set(permanent_value);
    permanent_value = nullptr;

    churn();

    check(stella_object_read_primitive(value.get(), 0) == 31415, "changed roots not observed");
}

}

int main(int argc, char **argv) {
    return runScenario(argc, argv, roots);
}
