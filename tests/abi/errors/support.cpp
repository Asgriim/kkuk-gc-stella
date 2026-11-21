#include "support.hpp"

#include <exception>
#include <iostream>

namespace gc_test {

void initializeCollector() {
    const StellaGcConfig config{STELLA_C_GC_ABI_VERSION, heap_limit};
    stella_gc_init(&config);
}

void accessField(
    std::string_view operation, StellaValue *object, StellaValue *value, std::size_t index) {
    if (operation == "init_managed") {
        stella_object_init_managed(*object, index, *value);
    } else if (operation == "init_primitive") {
        stella_object_init_primitive(*object, index, 7);
    } else if (operation == "read_managed") {
        *value = stella_gc_read_managed(object, index);
    } else if (operation == "write_managed") {
        stella_gc_write_managed(object, index, value);
    } else if (operation == "read_primitive") {
        static_cast<void>(stella_object_read_primitive(*object, index));
    } else if (operation == "write_primitive") {
        stella_object_write_primitive(*object, index, 7);
    } else {
        check(false, "unknown access operation");
    }
}

}

int main(int argc, char **argv) {
    using namespace gc_test;

    try {
        check(argc == 2 || argc == 3, "usage: SCENARIO CASE [HEAP_BYTES]");
        configureHeap(argc - 1, argv + 1);
        runErrorScenario(argv[1]);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "test error: " << error.what() << '\n';
        return 2;
    }
}
