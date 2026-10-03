#include <stella/scalar.h>

#define STELLA_SCALAR(symbol, label) \
  static const StellaObjectDescriptor symbol##_descriptor = { \
      .abi_version = STELLA_C_GC_ABI_VERSION, \
      .kind = STELLA_OBJECT_DATA, \
      .flags = STELLA_DESCRIPTOR_STATIC, \
      .entry_arity = 0, \
      .managed_count = 0, .primitive_count = 0, .slot_capacity = 0, \
      .entry_code = NULL, .debug_name = label}; \
  StellaObject symbol = { \
      .descriptor = &symbol##_descriptor, .gc_word = 0, .fields = NULL}

STELLA_SCALAR(stella_bool_false, "Bool::false");
STELLA_SCALAR(stella_bool_true, "Bool::true");
STELLA_SCALAR(stella_nat_zero, "Nat::zero");
STELLA_SCALAR(stella_unit, "Unit::unit");

const StellaObjectDescriptor stella_nat_succ_descriptor = {
    .abi_version = STELLA_C_GC_ABI_VERSION,
    .kind = STELLA_OBJECT_DATA,
    .flags = 0,
    .entry_arity = 0,
    .managed_count = 1, .primitive_count = 0, .slot_capacity = 1,
    .entry_code = NULL, .debug_name = "Nat::succ"};
