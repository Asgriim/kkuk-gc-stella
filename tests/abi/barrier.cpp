#include "support.hpp"

using namespace gc_test;

namespace {

void barrier() {
    Root owner;
    Root child;

    owner.allocate(pair_descriptor);
    untilMinor();

    const auto old_address = reinterpret_cast<std::uintptr_t>(owner.get());

    child.allocate(leaf_descriptor);
    stella_object_init_primitive(child.get(), 0, 4567);

    const auto before = statistics();
    stella_gc_write_managed(owner.slot(), 0, child.slot());
    stella_gc_write_managed(owner.slot(), 1, child.slot());

    check(
        statistics().at("write barrier activations") == before.at("write barrier activations") + 1,
        "owner must be remembered once");
    child.set(nullptr);
    untilMinor();

    check(reinterpret_cast<std::uintptr_t>(owner.get()) == old_address, "minor moved old owner");
    child.set(stella_gc_read_managed(owner.slot(), 0));
    check(stella_object_read_primitive(child.get(), 0) == 4567, "old-to-young edge lost");
    check(stella_gc_read_managed(owner.slot(), 1) == child.get(), "young child was duplicated");

    const auto unchanged = statistics().at("write barrier activations");
    stella_gc_write_managed(owner.slot(), 0, child.slot());

    check(
        statistics().at("write barrier activations") == unchanged, "old-to-old triggered barrier");
    child.allocate(leaf_descriptor);
    stella_object_init_primitive(child.get(), 0, 89);

    stella_gc_write_managed(owner.slot(), 0, child.slot());

    check(statistics().at("write barrier activations") == unchanged + 1,
        "owner was not re-remembered");
    child.set(nullptr);
    untilMinor();

    child.set(stella_gc_read_managed(owner.slot(), 0));
    check(stella_object_read_primitive(child.get(), 0) == 89, "second young edge lost");
}

}

int main(int argc, char **argv) {
    return runScenario(argc, argv, barrier);
}
