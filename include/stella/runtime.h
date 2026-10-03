#ifndef STELLA_C_GC_ABI_V1_RUNTIME_H
#define STELLA_C_GC_ABI_V1_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define STELLA_C_GC_ABI_VERSION UINT32_C(1)

typedef struct StellaObject StellaObject;
typedef struct StellaObjectDescriptor StellaObjectDescriptor;

typedef StellaObject *StellaValue;
typedef uintptr_t StellaPrimitive;

typedef union StellaSlot {
  StellaValue managed;
  StellaPrimitive primitive;
} StellaSlot;

typedef void (*StellaEntryCode)(
    StellaValue *result_root,
    StellaValue *self_root,
    size_t argument_count,
    StellaValue *const argument_roots[]);

enum StellaObjectKind {
  STELLA_OBJECT_DATA = 1,
  STELLA_OBJECT_CLOSURE = 2,
  STELLA_OBJECT_THUNK = 3,
  STELLA_OBJECT_INDIRECTION = 4
};

enum StellaDescriptorFlags {
  STELLA_DESCRIPTOR_STATIC = 1u << 0,
  STELLA_DESCRIPTOR_UPDATABLE = 1u << 1
};

struct StellaObjectDescriptor {
  uint32_t abi_version;
  uint32_t kind;
  uint32_t flags;
  uint32_t entry_arity;
  size_t managed_count;
  size_t primitive_count;
  size_t slot_capacity;
  StellaEntryCode entry_code;
  const char *debug_name;
};

struct StellaObject {
  const StellaObjectDescriptor *descriptor;
  uintptr_t gc_word;
  StellaSlot *fields;
};

_Static_assert(
    _Alignof(StellaObject) >= _Alignof(StellaSlot),
    "StellaObject must align its trailing slots");

_Noreturn void stella_gc_out_of_memory(size_t requested_bytes);
_Noreturn void stella_abi_violation(const char *message);

#endif
