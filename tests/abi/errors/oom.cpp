#include "support.hpp"

namespace gc_test {

void runErrorScenario(std::string_view name) {
    initializeCollector();

    Root object;
    Root value;

    if (name == "object") {
        auto huge = descriptor(0, 0, heap_limit / sizeof(StellaSlot) + 1);

        object.allocate(huge);
    } else if (name == "live") {
        for (std::size_t i = 0; i < heap_limit; ++i) {
            value.allocate(node_descriptor);
            stella_object_init_managed(value.get(), 0, object.get());
            stella_object_init_primitive(value.get(), 0, i);
            object.set(value.get());
        }
    } else {
        check(false, "unknown out-of-memory case");
    }
}

}
