#ifndef STELLA_C_GC_ABI_V1_GC_H
#define STELLA_C_GC_ABI_V1_GC_H

#include <stella/runtime.h>

typedef struct StellaGcConfig {
  uint32_t abi_version;
  size_t max_heap_bytes;
} StellaGcConfig;

void stella_gc_init(const StellaGcConfig *config);
void stella_gc_shutdown(void);

void stella_gc_push_root(StellaValue *root);
void stella_gc_pop_root(StellaValue *root);
void stella_gc_register_permanent_root(StellaValue *root);

StellaValue stella_gc_alloc(const StellaObjectDescriptor *descriptor);

void stella_object_init_managed(
    StellaValue object, size_t index, StellaValue value);

void stella_object_init_primitive(
    StellaValue object, size_t index, StellaPrimitive value);

StellaValue stella_gc_read_managed(
    StellaValue *object_root, size_t index);

void stella_gc_write_managed(
    StellaValue *object_root, size_t index, StellaValue *value_root);

StellaPrimitive stella_object_read_primitive(
    StellaValue object, size_t index);

void stella_object_write_primitive(
    StellaValue object, size_t index, StellaPrimitive value);

void stella_gc_update_object(
    StellaValue *object_root,
    const StellaObjectDescriptor *new_descriptor,
    StellaValue *const new_managed_roots[],
    const StellaPrimitive new_primitives[]);

void stella_gc_print_statistics(FILE *output);
void stella_gc_print_state(FILE *output);
void stella_gc_print_roots(FILE *output);

#endif
