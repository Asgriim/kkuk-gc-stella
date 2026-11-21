#include "support.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>

namespace gc_test {

void check(bool condition, std::string_view message, std::source_location location) {
    if (!condition) {
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
        std::exit(2);
    }
}

StellaObject static_leaf{&static_leaf_descriptor, 17, nullptr};
std::array<StellaSlot, 2> static_fields{
    StellaSlot{.managed = &static_leaf}, StellaSlot{.primitive = 12345}};
StellaObject static_object{&static_descriptor, 29, static_fields.data()};
std::size_t heap_limit = 2048;

std::size_t objectBytes(const StellaObjectDescriptor &value) {
    return sizeof(StellaObject) + value.slot_capacity * sizeof(StellaSlot);
}

std::string capture(void (*print)(FILE *)) {
    const auto close_file = [](FILE *file) { std::fclose(file); };
    std::unique_ptr<FILE, decltype(close_file)> file(std::tmpfile(), close_file);

    check(file != nullptr, "tmpfile failed");

    print(file.get());

    check(std::fflush(file.get()) == 0, "fflush failed");
    check(std::fseek(file.get(), 0, SEEK_SET) == 0, "fseek failed");

    std::string result;
    std::array<char, 4096> buffer{};

    while (auto count = std::fread(buffer.data(), 1, buffer.size(), file.get())) {
        result.append(buffer.data(), count);
    }

    check(std::ferror(file.get()) == 0, "fread failed");
    return result;
}

Statistics statistics() {
    Statistics result;
    std::istringstream input(capture(stella_gc_print_statistics));
    std::string line;

    while (std::getline(input, line)) {
        const auto separator = line.find(": ");
        check(separator != std::string::npos, "malformed statistics line");
        result.emplace(line.substr(0, separator), std::stoull(line.substr(separator + 2)));
    }

    return result;
}

void checkLayout(StellaValue object) {
    const auto *layout = object->descriptor;

    if (layout->slot_capacity != 0) {
        check(object->fields == reinterpret_cast<StellaSlot *>(object + 1), "fields not relocated");
    }

    for (auto i = layout->managed_count + layout->primitive_count; i < layout->slot_capacity; ++i) {
        check(object->fields[i].primitive == 0, "inactive slot is not zero");
    }
}

void churn(std::size_t count) {
    Root garbage;

    for (std::size_t i = 0; i < count; ++i) {
        garbage.allocate(leaf_descriptor);
        stella_object_init_primitive(garbage.get(), 0, i);
    }
}

void untilMinor() {
    const auto before = statistics();
    Root garbage;

    for (std::size_t i = 0; i < heap_limit; ++i) {
        garbage.allocate(leaf_descriptor);
        stella_object_init_primitive(garbage.get(), 0, i);

        const auto after = statistics();
        check(after.at("full collections") == before.at("full collections"), "unexpected full GC");
        if (after.at("minor collections") > before.at("minor collections")) {
            return;
        }
    }

    check(false, "minor GC never ran");
}

void configureHeap(int argc, char **argv) {
    check(argc == 1 || argc == 2, "usage: SCENARIO [HEAP_BYTES]");
    if (argc == 2)
        heap_limit = std::stoull(argv[1]);
}

int runScenario(int argc, char **argv, void (*scenario)()) {
    try {
        configureHeap(argc, argv);
        const StellaGcConfig config{STELLA_C_GC_ABI_VERSION, heap_limit};
        stella_gc_init(&config);

        scenario();
        stella_gc_shutdown();

        std::cout << "ok\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "test error: " << error.what() << '\n';
        return 2;
    }
}

}
