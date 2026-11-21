#include "support.hpp"

namespace gc_test {
namespace {

void operation(std::string_view name) {
    StellaValue object = &static_object;
    StellaValue value = nullptr;
    const StellaGcConfig config{STELLA_C_GC_ABI_VERSION, heap_limit};

    if (name == "init") {
        stella_gc_init(&config);
    } else if (name == "shutdown") {
        stella_gc_shutdown();
    } else if (name == "alloc") {
        object = stella_gc_alloc(&leaf_descriptor);
    } else if (name == "push_root") {
        stella_gc_push_root(&object);
    } else if (name == "pop_root") {
        stella_gc_pop_root(&object);
    } else if (name == "permanent_root") {
        stella_gc_register_permanent_root(&object);
    } else if (name == "update") {
        stella_gc_update_object(&object, &cleared_descriptor, nullptr, nullptr);
    } else if (name == "statistics") {
        stella_gc_print_statistics(stdout);
    } else if (name == "state") {
        stella_gc_print_state(stdout);
    } else if (name == "roots") {
        stella_gc_print_roots(stdout);
    } else {
        accessField(name, &object, &value, 0);
    }
}

}

void runErrorScenario(std::string_view name) {
    const StellaGcConfig config{STELLA_C_GC_ABI_VERSION, heap_limit};

    if (name.starts_with("before/")) {
        operation(name.substr(7));
    } else if (name.starts_with("after/")) {
        stella_gc_init(&config);
        stella_gc_shutdown();
        operation(name.substr(6));
    } else if (name == "repeat_init") {
        stella_gc_init(&config);
        operation("init");
    } else {
        check(false, "unknown lifecycle error");
    }
}

}
