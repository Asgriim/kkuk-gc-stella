#include "support.hpp"

using namespace gc_test;

namespace {

void halfHeap() {
    check(17 * objectBytes(node_descriptor) <= heap_limit / 2, "half-heap test configuration");

    Root head;
    Root next;
    Root cursor;

    for (std::size_t round = 0; round < 250; ++round) {
        head.set(nullptr);
        next.set(nullptr);
        cursor.set(nullptr);

        for (std::size_t i = 0; i < 16; ++i) {
            next.allocate(node_descriptor);
            stella_object_init_managed(next.get(), 0, head.get());
            stella_object_init_primitive(next.get(), 0, round * 16 + i);
            head.set(next.get());
        }

        cursor.set(head.get());

        for (std::size_t i = 16; i > 0; --i) {
            check(cursor.get() != nullptr, "list shortened");
            check(stella_object_read_primitive(cursor.get(), 0) == round * 16 + i - 1,
                "list corrupted");
            cursor.set(stella_gc_read_managed(cursor.slot(), 0));
        }

        check(cursor.get() == nullptr, "unexpected list tail");
    }

    const auto stats = statistics();

    check(stats.at("requested bytes") > heap_limit * 10, "insufficient allocation pressure");
    check(stats.at("collections") > 1, "repeated collection not exercised");
    check(stats.at("maximum occupied bytes") <= heap_limit, "heap limit exceeded");
}

}

int main(int argc, char **argv) {
    return runScenario(argc, argv, halfHeap);
}
