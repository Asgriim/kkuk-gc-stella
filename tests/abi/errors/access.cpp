#include "support.hpp"

namespace gc_test {

void runErrorScenario(std::string_view name) {
    initializeCollector();

    Root object;
    Root value;

    if (name.starts_with("null/")) {
        accessField(name.substr(5), object.slot(), value.slot(), 0);
    } else if (name.starts_with("index/")) {
        object.allocate(mixed_descriptor);
        accessField(name.substr(6), object.slot(), value.slot(), 2);
    } else if (name.starts_with("static/")) {
        object.set(&static_object);
        accessField(name.substr(7), object.slot(), value.slot(), 0);
    } else {
        check(false, "unknown access error");
    }
}

}
