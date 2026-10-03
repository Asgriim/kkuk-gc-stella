#ifndef STELLA_SCALAR_H
#define STELLA_SCALAR_H

#include <stella/runtime.h>

/* Представление значений Bool, Nat и Unit, общее для порождённого кода и runtime/main.c. */
extern StellaObject stella_bool_false;
extern StellaObject stella_bool_true;
extern StellaObject stella_nat_zero;
extern StellaObject stella_unit;
extern const StellaObjectDescriptor stella_nat_succ_descriptor;

#endif
