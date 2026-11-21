#include "support.hpp"

namespace gc_test {

void runErrorScenario(std::string_view name) {
    if (name == "null") {
        stella_gc_init(nullptr);
        return;
    }

    StellaGcConfig config{STELLA_C_GC_ABI_VERSION, heap_limit};

    if (name == "version") {
        config.abi_version = 2;
    } else if (name == "zero") {
        config.max_heap_bytes = 0;
    } else {
        check(name == "environment", "unknown configuration error");
    }

    stella_gc_init(&config);
}

}
