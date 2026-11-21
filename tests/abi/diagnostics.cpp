#include "support.hpp"

using namespace gc_test;

namespace {

StellaValue permanent_value = nullptr;

void diagnostics() {
    check(statistics().at("allocated objects") == 0, "nonzero initial statistics");

    Root object;
    Root value;
    permanent_value = nullptr;
    stella_gc_register_permanent_root(&permanent_value);

    object.allocate(mixed_descriptor);
    const auto address = reinterpret_cast<std::uintptr_t>(object.get());

    const auto before = statistics();
    stella_object_init_managed(object.get(), 0, &static_leaf);
    stella_object_init_managed(object.get(), 1, nullptr);
    stella_object_init_primitive(object.get(), 0, 11);
    stella_object_init_primitive(object.get(), 1, 22);

    check(statistics() == before, "initialization changed access counters");

    stella_object_write_primitive(object.get(), 0, 33);

    check(stella_object_read_primitive(object.get(), 0) == 33, "primitive write failed");

    StellaValue extra = object.get();
    stella_gc_push_root(&extra);

    const auto before_print = statistics();
    const auto state = capture(stella_gc_print_state);
    const auto roots_text = capture(stella_gc_print_roots);

    check(statistics() == before_print, "printing changed statistics");
    check(state.find("inactive[4]=0") != std::string::npos, "inactive slots missing from dump");
    check(state.find("primitive[0]=33") != std::string::npos, "primitive missing from dump");
    check(state.find("managed[0]=") != std::string::npos, "managed field missing from dump");

    for (auto label : {"young=[", "old=[", "reserve=[", "next=", "limit=", "remembered objects:"}) {
        check(state.find(label) != std::string::npos, "incomplete state output");
    }

    for (auto label : {"dynamic root 0: slot=", "permanent root 0: slot=", " value="}) {
        check(roots_text.find(label) != std::string::npos, "incomplete roots output");
    }

    stella_gc_pop_root(&extra);

    check(reinterpret_cast<std::uintptr_t>(object.get()) == address, "non-safepoint moved object");
    check(statistics().at("collections") == before.at("collections"), "non-safepoint collected");

    stella_gc_write_managed(object.slot(), 1, value.slot());

    check(stella_gc_read_managed(object.slot(), 1) == nullptr, "null write failed");

    const auto after = statistics();
    check(
        after.at("requested bytes") == objectBytes(mixed_descriptor), "allocation bytes incorrect");
    check(after.at("occupied bytes") == objectBytes(mixed_descriptor), "occupied bytes incorrect");
    check(after.at("allocated objects") == 1, "object count incorrect");

    for (auto label : {"managed reads", "managed writes", "primitive reads", "primitive writes"}) {
        check(after.at(label) == 1, "access counter incorrect");
    }

    check(
        after.at("dynamic roots") == 2 && after.at("permanent roots") == 1, "root count incorrect");
    check(after.at("maximum dynamic roots") == 3, "maximum root count incorrect");

    churn();

    const auto final = statistics();

    check(final.at("requested bytes") ==
              objectBytes(mixed_descriptor) + 400 * objectBytes(leaf_descriptor),
        "copying counted as allocation");
    check(final.at("allocated objects") == 401, "copying counted as new objects");
    check(final.at("occupied bytes") ==
              final.at("young occupied bytes") + final.at("old occupied bytes"),
        "generation occupancy mismatch");
    check(final.at("collections") == final.at("minor collections") + final.at("full collections"),
        "collection count mismatch");
    for (const std::string prefix : {"", "young ", "old "}) {
        check(
            final.at(prefix + "occupied bytes") <= final.at("maximum " + prefix + "occupied bytes"),
            "maximum occupancy too small");
        check(final.at("maximum " + prefix + "occupied bytes") <= heap_limit,
            "maximum exceeds limit");
    }

    check(final.at("managed reads") == 1 && final.at("primitive writes") == 1,
        "GC changed access counters");
}

}

int main(int argc, char **argv) {
    return runScenario(argc, argv, diagnostics);
}
