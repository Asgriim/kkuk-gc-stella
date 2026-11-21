#include "support.hpp"

namespace gc_test {

void runErrorScenario(std::string_view name) {
    initializeCollector();

    if (name == "empty_pop") {
        StellaValue value = nullptr;
        stella_gc_pop_root(&value);
        return;
    }

    Root object;
    Root value;

    if (name == "shutdown_roots") {
        stella_gc_shutdown();
    } else if (name == "root_order") {
        stella_gc_pop_root(object.slot());
    } else if (name == "duplicate_root") {
        stella_gc_push_root(object.slot());
    } else if (name == "duplicate_permanent") {
        stella_gc_register_permanent_root(object.slot());
    } else if (name == "null_root") {
        stella_gc_push_root(nullptr);
    } else {
        check(false, "unknown root error");
    }
}

}
