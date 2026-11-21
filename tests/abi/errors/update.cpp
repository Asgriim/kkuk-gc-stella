#include "support.hpp"

namespace gc_test {

void runErrorScenario(std::string_view name) {
    initializeCollector();

    Root object;
    Root value;

    if (name == "null_object") {
        stella_gc_update_object(object.slot(), &cleared_descriptor, nullptr, nullptr);
        return;
    }

    object.allocate(name == "immutable" ? leaf_descriptor : pair_descriptor);

    std::array<StellaValue *, 1> values{value.slot()};
    std::array<StellaPrimitive, 2> primitives{1, 2};
    const auto *layout = &changed_descriptor;
    StellaValue *const *managed = values.data();
    const StellaPrimitive *primitive = primitives.data();

    if (name == "immutable") {
        layout = &leaf_descriptor;
    } else if (name == "static") {
        layout = &static_update_descriptor;
    } else if (name == "capacity") {
        layout = &leaf_descriptor;
    } else if (name == "null_descriptor") {
        layout = nullptr;
    } else if (name == "missing_managed") {
        managed = nullptr;
    } else if (name == "missing_primitive") {
        primitive = nullptr;
    } else {
        check(false, "unknown update error");
    }

    stella_gc_update_object(object.slot(), layout, managed, primitive);
}

}
