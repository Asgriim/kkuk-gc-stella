#include "support.hpp"

using namespace gc_test;

namespace {

void update() {
    Root object;
    Root alias;
    Root child;

    child.allocate(leaf_descriptor);
    stella_object_init_primitive(child.get(), 0, 555);
    object.allocate(pair_descriptor);
    stella_object_init_managed(object.get(), 0, child.get());
    stella_object_init_managed(object.get(), 1, object.get());
    stella_object_init_primitive(object.get(), 0, 123);
    alias.set(object.get());

    std::array<StellaValue *, 1> values{child.slot()};
    std::array<StellaPrimitive, 2> primitives{77, 88};
    const auto before = statistics();
    stella_gc_update_object(object.slot(), &changed_descriptor, values.data(), primitives.data());

    const auto after = statistics();
    check(after.at("managed writes") == before.at("managed writes") + 1, "update managed count");
    check(after.at("primitive writes") == before.at("primitive writes") + 2,
        "update primitive count");
    check(after.at("allocated objects") == before.at("allocated objects"),
        "update allocated an object");
    child.set(nullptr);
    churn();

    check(object.get() == alias.get(), "update broke identity");
    check(object.get()->descriptor == &changed_descriptor, "new descriptor lost");
    child.set(stella_gc_read_managed(alias.slot(), 0));
    check(stella_object_read_primitive(child.get(), 0) == 555, "update lost new managed value");
    check(stella_object_read_primitive(alias.get(), 0) == 77, "update primitive offset");
    check(stella_object_read_primitive(alias.get(), 1) == 88, "update primitive value");

    stella_gc_update_object(object.slot(), &cleared_descriptor, nullptr, nullptr);
    checkLayout(object.get());

    for (const auto *layout : {&closure_descriptor, &thunk_descriptor, &indirection_descriptor}) {
        stella_gc_update_object(object.slot(), layout, values.data(), nullptr);

        churn();

        check(object.get() == alias.get(), "kind change broke identity");
        check(stella_gc_read_managed(object.slot(), 0) == child.get(), "executable capture lost");
        checkLayout(object.get());
    }
}

}

int main(int argc, char **argv) {
    return runScenario(argc, argv, update);
}
