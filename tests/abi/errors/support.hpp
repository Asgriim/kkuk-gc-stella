#pragma once

#include "../support.hpp"

namespace gc_test {

void initializeCollector();

void accessField(
    std::string_view operation, StellaValue *object, StellaValue *value, std::size_t index);

void runErrorScenario(std::string_view name);

}
