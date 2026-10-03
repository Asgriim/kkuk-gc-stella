/* Заглушка сборщика мусора: полностью реализует интерфейс из stella/gc.h, проверяет аргументы,
 * считает статистику и соблюдает ограничение памяти, но освобождает память только при
 * завершении работы. Её можно взять за основу или заменить целиком.
 * Макросы GC_BEFORE_*, GC_WRITE_BARRIER и STELLA_GC_SUPPORT --- места расширения, которые
 * в самой заглушке ничего не делают. */
#include <stella/gc.h>

#include "internal/gc_stats.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct Allocation Allocation;

struct Allocation {
  StellaValue object;
  size_t requested_bytes;
  size_t rounded_bytes;
  unsigned char *initialised;
#ifdef STELLA_GC_ALLOCATION_FIELDS
  STELLA_GC_ALLOCATION_FIELDS
#endif
  Allocation *next;
};

enum Lifecycle {
  LIFECYCLE_FRESH,
  LIFECYCLE_ACTIVE,
  LIFECYCLE_FINISHED
};

static enum Lifecycle lifecycle = LIFECYCLE_FRESH;
static size_t max_heap_bytes;
static Allocation *allocations;
static StellaValue **dynamic_roots;
static size_t dynamic_root_depth;
static size_t dynamic_root_capacity;
static StellaValue **permanent_roots;
static size_t permanent_root_count;
static size_t permanent_root_capacity;
static StellaGcStats counters;

static void require_active(void) {
  if (lifecycle != LIFECYCLE_ACTIVE) {
    stella_abi_violation("collector operation outside its active lifetime");
  }
}

static bool add_overflows(size_t left, size_t right) {
  return left > SIZE_MAX - right;
}

static bool multiply_overflows(size_t left, size_t right) {
  return right != 0 && left > SIZE_MAX / right;
}

static size_t object_size(const StellaObjectDescriptor *descriptor) {
  if (multiply_overflows(descriptor->slot_capacity, sizeof(StellaSlot))) {
    stella_abi_violation("object slot capacity overflows size_t");
  }
  size_t slots = descriptor->slot_capacity * sizeof(StellaSlot);
  if (add_overflows(sizeof(StellaObject), slots)) {
    stella_abi_violation("object size overflows size_t");
  }
  return sizeof(StellaObject) + slots;
}

static size_t rounded_size(size_t size) {
  size_t alignment = _Alignof(max_align_t);
  size_t remainder = size % alignment;
  if (remainder == 0) {
    return size;
  }
  size_t padding = alignment - remainder;
  if (add_overflows(size, padding)) {
    stella_abi_violation("aligned object size overflows size_t");
  }
  return size + padding;
}

static void validate_descriptor(const StellaObjectDescriptor *descriptor) {
  if (descriptor == NULL) {
    stella_abi_violation("null object descriptor");
  }
  if (descriptor->abi_version != STELLA_C_GC_ABI_VERSION) {
    stella_abi_violation("incompatible descriptor ABI version");
  }
  if (descriptor->kind < STELLA_OBJECT_DATA ||
      descriptor->kind > STELLA_OBJECT_INDIRECTION) {
    stella_abi_violation("unknown object kind");
  }
  uint32_t known_flags = STELLA_DESCRIPTOR_STATIC |
                         STELLA_DESCRIPTOR_UPDATABLE;
  if ((descriptor->flags & ~known_flags) != 0) {
    stella_abi_violation("unknown descriptor flag");
  }
  if ((descriptor->flags & STELLA_DESCRIPTOR_STATIC) != 0 &&
      (descriptor->flags & STELLA_DESCRIPTOR_UPDATABLE) != 0) {
    stella_abi_violation("static descriptor is marked updatable");
  }
  if (add_overflows(
          descriptor->managed_count,
          descriptor->primitive_count)) {
    stella_abi_violation("active field count overflows size_t");
  }
  if (descriptor->managed_count + descriptor->primitive_count >
      descriptor->slot_capacity) {
    stella_abi_violation("active fields exceed slot capacity");
  }
  if ((descriptor->kind == STELLA_OBJECT_CLOSURE ||
       descriptor->kind == STELLA_OBJECT_THUNK) &&
      descriptor->entry_code == NULL) {
    stella_abi_violation("executable descriptor has no entry code");
  }
  if ((descriptor->kind == STELLA_OBJECT_DATA ||
       descriptor->kind == STELLA_OBJECT_INDIRECTION) &&
      descriptor->entry_code != NULL) {
    stella_abi_violation("non-executable descriptor has entry code");
  }
  if (descriptor->kind == STELLA_OBJECT_THUNK &&
      descriptor->entry_arity != 0) {
    stella_abi_violation("thunk descriptor has non-zero entry arity");
  }
  if ((descriptor->kind == STELLA_OBJECT_DATA ||
       descriptor->kind == STELLA_OBJECT_INDIRECTION) &&
      descriptor->entry_arity != 0) {
    stella_abi_violation("non-executable descriptor has non-zero entry arity");
  }
}

static Allocation *find_allocation(StellaValue object) {
  for (Allocation *allocation = allocations;
       allocation != NULL;
       allocation = allocation->next) {
    if (allocation->object == object) {
      return allocation;
    }
  }
  return NULL;
}

static Allocation *require_heap_object(StellaValue object) {
  if (object == NULL) {
    stella_abi_violation("null object");
  }
  Allocation *allocation = find_allocation(object);
  if (allocation == NULL) {
    stella_abi_violation("object is not owned by the collector");
  }
  validate_descriptor(object->descriptor);
  return allocation;
}

static void validate_object(StellaValue object) {
  if (object == NULL) {
    stella_abi_violation("null object");
  }
  validate_descriptor(object->descriptor);
  if ((object->descriptor->flags & STELLA_DESCRIPTOR_STATIC) == 0 &&
      find_allocation(object) == NULL) {
    stella_abi_violation("heap object is not owned by the collector");
  }
}

static bool root_is_registered(StellaValue *root) {
  for (size_t i = 0; i < dynamic_root_depth; ++i) {
    if (dynamic_roots[i] == root) {
      return true;
    }
  }
  for (size_t i = 0; i < permanent_root_count; ++i) {
    if (permanent_roots[i] == root) {
      return true;
    }
  }
  return false;
}

static void require_registered_root(StellaValue *root) {
  if (root == NULL || !root_is_registered(root)) {
    stella_abi_violation("field access uses an unregistered root");
  }
}

static void grow_roots(
    StellaValue ***roots,
    size_t *capacity,
    size_t required) {
  if (required <= *capacity) {
    return;
  }
  if (*capacity > SIZE_MAX / 2) {
    stella_abi_violation("root stack capacity overflows size_t");
  }
  size_t next = *capacity == 0 ? 8 : *capacity * 2;
  if (next < required || multiply_overflows(next, sizeof(**roots))) {
    stella_abi_violation("root stack capacity overflows size_t");
  }
  void *storage = realloc(*roots, next * sizeof(**roots));
  if (storage == NULL) {
    stella_abi_violation("root stack exhaustion");
  }
  *roots = storage;
  *capacity = next;
}

void stella_gc_init(const StellaGcConfig *config) {
  if (lifecycle != LIFECYCLE_FRESH) {
    stella_abi_violation("collector initialised more than once");
  }
  if (config == NULL) {
    stella_abi_violation("null collector configuration");
  }
  if (config->abi_version != STELLA_C_GC_ABI_VERSION) {
    stella_abi_violation("incompatible collector ABI version");
  }
  if (config->max_heap_bytes == 0) {
    stella_abi_violation("zero maximum heap size");
  }
  max_heap_bytes = config->max_heap_bytes;
  lifecycle = LIFECYCLE_ACTIVE;
}

void stella_gc_shutdown(void) {
  require_active();
  if (dynamic_root_depth != 0) {
    stella_abi_violation("runtime shutdown with dynamic roots registered");
  }
  while (allocations != NULL) {
    Allocation *next = allocations->next;
    free(allocations->initialised);
    free(allocations->object);
    free(allocations);
    allocations = next;
  }
  free(dynamic_roots);
  free(permanent_roots);
  dynamic_roots = NULL;
  permanent_roots = NULL;
  dynamic_root_capacity = 0;
  permanent_root_capacity = 0;
  counters.occupied_bytes = 0;
  lifecycle = LIFECYCLE_FINISHED;
}

void stella_gc_push_root(StellaValue *root) {
  require_active();
  if (root == NULL) {
    stella_abi_violation("null root slot");
  }
  if (root_is_registered(root)) {
    stella_abi_violation("duplicate root registration");
  }
  grow_roots(&dynamic_roots, &dynamic_root_capacity, dynamic_root_depth + 1);
  dynamic_roots[dynamic_root_depth++] = root;
  counters.dynamic_root_depth = dynamic_root_depth;
  if (dynamic_root_depth > counters.maximum_dynamic_root_depth) {
    counters.maximum_dynamic_root_depth = dynamic_root_depth;
  }
}

void stella_gc_pop_root(StellaValue *root) {
  require_active();
  if (dynamic_root_depth == 0) {
    stella_abi_violation("dynamic root stack underflow");
  }
  if (dynamic_roots[dynamic_root_depth - 1] != root) {
    stella_abi_violation("dynamic roots popped out of order");
  }
  --dynamic_root_depth;
  counters.dynamic_root_depth = dynamic_root_depth;
}

void stella_gc_register_permanent_root(StellaValue *root) {
  require_active();
  if (root == NULL) {
    stella_abi_violation("null permanent-root slot");
  }
  if (root_is_registered(root)) {
    stella_abi_violation("duplicate root registration");
  }
  grow_roots(
      &permanent_roots,
      &permanent_root_capacity,
      permanent_root_count + 1);
  permanent_roots[permanent_root_count++] = root;
  counters.permanent_root_count = permanent_root_count;
}

#ifdef STELLA_GC_SUPPORT
#include STELLA_GC_SUPPORT
#endif

#ifndef GC_BEFORE_ALLOC
#define GC_BEFORE_ALLOC(requested) ((void)0)
#define GC_BEFORE_READ(root) ((void)0)
#define GC_BEFORE_WRITE(root) ((void)0)
#define GC_BEFORE_UPDATE(root) ((void)0)
#define GC_WRITE_BARRIER(owner, value) ((void)0)
#define GC_ALLOCATION_INIT(allocation) ((void)0)
#define GC_DESCRIPTION "malloc stub"
#endif

StellaValue stella_gc_alloc(const StellaObjectDescriptor *descriptor) {
  require_active();
  validate_descriptor(descriptor);
  if ((descriptor->flags & STELLA_DESCRIPTOR_STATIC) != 0) {
    stella_abi_violation("heap allocation requested with static descriptor");
  }
  size_t requested = object_size(descriptor);
  size_t rounded = rounded_size(requested);
  GC_BEFORE_ALLOC(requested);
  if (requested > max_heap_bytes - counters.occupied_bytes) {
    ++counters.failed_allocations;
    stella_gc_out_of_memory(requested);
  }

  StellaValue object = malloc(rounded);
  Allocation *allocation = malloc(sizeof(*allocation));
  unsigned char *initialised = NULL;
  if (descriptor->slot_capacity != 0) {
    initialised = calloc(descriptor->slot_capacity, sizeof(*initialised));
  }
  if (object == NULL || allocation == NULL ||
      (descriptor->slot_capacity != 0 && initialised == NULL)) {
    free(object);
    free(allocation);
    free(initialised);
    ++counters.failed_allocations;
    stella_gc_out_of_memory(requested);
  }

  object->descriptor = descriptor;
  object->gc_word = 0;
  object->fields = descriptor->slot_capacity == 0
      ? NULL
      : (StellaSlot *)(object + 1);
  for (size_t i = 0; i < descriptor->managed_count; ++i) {
    object->fields[i].managed = NULL;
  }
  for (size_t i = descriptor->managed_count;
       i < descriptor->slot_capacity;
       ++i) {
    object->fields[i].primitive = 0;
  }

  allocation->object = object;
  allocation->requested_bytes = requested;
  allocation->rounded_bytes = rounded;
  allocation->initialised = initialised;
  GC_ALLOCATION_INIT(allocation);
  allocation->next = allocations;
  allocations = allocation;

  counters.requested_bytes += requested;
  counters.rounded_bytes += rounded;
  ++counters.allocated_objects;
  counters.occupied_bytes += requested;
  if (counters.occupied_bytes > counters.maximum_occupied_bytes) {
    counters.maximum_occupied_bytes = counters.occupied_bytes;
  }
  return object;
}

void stella_object_init_managed(
    StellaValue object,
    size_t index,
    StellaValue value) {
  require_active();
  Allocation *allocation = require_heap_object(object);
  if (index >= object->descriptor->managed_count) {
    stella_abi_violation("managed initialisation index out of range");
  }
  if (allocation->initialised[index] != 0) {
    stella_abi_violation("managed field initialised more than once");
  }
  object->fields[index].managed = value;
  allocation->initialised[index] = 1;
}

void stella_object_init_primitive(
    StellaValue object,
    size_t index,
    StellaPrimitive value) {
  require_active();
  Allocation *allocation = require_heap_object(object);
  if (index >= object->descriptor->primitive_count) {
    stella_abi_violation("primitive initialisation index out of range");
  }
  size_t physical = object->descriptor->managed_count + index;
  if (allocation->initialised[physical] != 0) {
    stella_abi_violation("primitive field initialised more than once");
  }
  object->fields[physical].primitive = value;
  allocation->initialised[physical] = 1;
}

StellaValue stella_gc_read_managed(
    StellaValue *object_root,
    size_t index) {
  require_active();
  require_registered_root(object_root);
  GC_BEFORE_READ(object_root);
  validate_object(*object_root);
  if (index >= (*object_root)->descriptor->managed_count) {
    stella_abi_violation("managed read index out of range");
  }
  ++counters.managed_reads;
  return (*object_root)->fields[index].managed;
}

void stella_gc_write_managed(
    StellaValue *object_root,
    size_t index,
    StellaValue *value_root) {
  require_active();
  require_registered_root(object_root);
  require_registered_root(value_root);
  GC_BEFORE_WRITE(object_root);
  validate_object(*object_root);
  if (((*object_root)->descriptor->flags & STELLA_DESCRIPTOR_STATIC) != 0) {
    stella_abi_violation("managed write to static object");
  }
  if (index >= (*object_root)->descriptor->managed_count) {
    stella_abi_violation("managed write index out of range");
  }
  GC_WRITE_BARRIER(*object_root, *value_root);
  (*object_root)->fields[index].managed = *value_root;
  ++counters.managed_writes;
}

StellaPrimitive stella_object_read_primitive(
    StellaValue object,
    size_t index) {
  require_active();
  validate_object(object);
  if (index >= object->descriptor->primitive_count) {
    stella_abi_violation("primitive read index out of range");
  }
  ++counters.primitive_reads;
  return object->fields[object->descriptor->managed_count + index].primitive;
}

void stella_object_write_primitive(
    StellaValue object,
    size_t index,
    StellaPrimitive value) {
  require_active();
  validate_object(object);
  if ((object->descriptor->flags & STELLA_DESCRIPTOR_STATIC) != 0) {
    stella_abi_violation("primitive write to static object");
  }
  if (index >= object->descriptor->primitive_count) {
    stella_abi_violation("primitive write index out of range");
  }
  object->fields[object->descriptor->managed_count + index].primitive = value;
  ++counters.primitive_writes;
}

void stella_gc_update_object(
    StellaValue *object_root,
    const StellaObjectDescriptor *new_descriptor,
    StellaValue *const new_managed_roots[],
    const StellaPrimitive new_primitives[]) {
  require_active();
  require_registered_root(object_root);
  Allocation *allocation = require_heap_object(*object_root);
  const StellaObjectDescriptor *old_descriptor = (*object_root)->descriptor;
  validate_descriptor(new_descriptor);
  if ((old_descriptor->flags & STELLA_DESCRIPTOR_UPDATABLE) == 0 ||
      (new_descriptor->flags & STELLA_DESCRIPTOR_STATIC) != 0) {
    stella_abi_violation("incompatible object-layout update flags");
  }
  if (old_descriptor->slot_capacity != new_descriptor->slot_capacity) {
    stella_abi_violation("object-layout update changes slot capacity");
  }
  if (new_descriptor->managed_count != 0 && new_managed_roots == NULL) {
    stella_abi_violation("missing managed values for object-layout update");
  }
  if (new_descriptor->primitive_count != 0 && new_primitives == NULL) {
    stella_abi_violation("missing primitive values for object-layout update");
  }
  for (size_t i = 0; i < new_descriptor->managed_count; ++i) {
    require_registered_root(new_managed_roots[i]);
  }

  GC_BEFORE_UPDATE(object_root);
  allocation = require_heap_object(*object_root);

  for (size_t i = 0; i < new_descriptor->managed_count; ++i) {
    GC_WRITE_BARRIER(*object_root, *new_managed_roots[i]);
    (*object_root)->fields[i].managed = *new_managed_roots[i];
    allocation->initialised[i] = 1;
    ++counters.managed_writes;
  }
  for (size_t i = 0; i < new_descriptor->primitive_count; ++i) {
    size_t physical = new_descriptor->managed_count + i;
    (*object_root)->fields[physical].primitive = new_primitives[i];
    allocation->initialised[physical] = 1;
    ++counters.primitive_writes;
  }
  size_t active = new_descriptor->managed_count +
                  new_descriptor->primitive_count;
  for (size_t i = active; i < new_descriptor->slot_capacity; ++i) {
    (*object_root)->fields[i].primitive = 0;
    allocation->initialised[i] = 0;
  }
  (*object_root)->descriptor = new_descriptor;
}

void stella_gc_print_statistics(FILE *output) {
  require_active();
  if (output == NULL) {
    stella_abi_violation("null statistics output stream");
  }
  fprintf(output, "requested bytes: %zu\n", counters.requested_bytes);
  fprintf(output, "rounded bytes: %zu\n", counters.rounded_bytes);
  fprintf(output, "allocated objects: %zu\n", counters.allocated_objects);
  fprintf(output, "collections: %zu\n", counters.collections);
  fprintf(output, "occupied bytes: %zu\n", counters.occupied_bytes);
  fprintf(
      output,
      "maximum occupied bytes: %zu\n",
      counters.maximum_occupied_bytes);
  fprintf(output, "managed reads: %zu\n", counters.managed_reads);
  fprintf(output, "managed writes: %zu\n", counters.managed_writes);
  fprintf(output, "primitive reads: %zu\n", counters.primitive_reads);
  fprintf(output, "primitive writes: %zu\n", counters.primitive_writes);
  fprintf(
      output,
      "read barrier activations: %zu\n",
      counters.read_barrier_activations);
  fprintf(
      output,
      "write barrier activations: %zu\n",
      counters.write_barrier_activations);
  fprintf(output, "dynamic roots: %zu\n", dynamic_root_depth);
  fprintf(
      output,
      "maximum dynamic roots: %zu\n",
      counters.maximum_dynamic_root_depth);
  fprintf(output, "permanent roots: %zu\n", permanent_root_count);
  fprintf(output, "failed allocations: %zu\n", counters.failed_allocations);
}

void stella_gc_print_roots(FILE *output) {
  require_active();
  if (output == NULL) {
    stella_abi_violation("null roots output stream");
  }
  for (size_t i = 0; i < dynamic_root_depth; ++i) {
    fprintf(
        output,
        "dynamic root %zu: slot=%p value=%p\n",
        i,
        (void *)dynamic_roots[i],
        (void *)*dynamic_roots[i]);
  }
  for (size_t i = 0; i < permanent_root_count; ++i) {
    fprintf(
        output,
        "permanent root %zu: slot=%p value=%p\n",
        i,
        (void *)permanent_roots[i],
        (void *)*permanent_roots[i]);
  }
}

void stella_gc_print_state(FILE *output) {
  require_active();
  if (output == NULL) {
    stella_abi_violation("null state output stream");
  }
  fprintf(
      output,
      "%s: occupied=%zu free=%zu limit=%zu\n",
      GC_DESCRIPTION,
      counters.occupied_bytes,
      max_heap_bytes - counters.occupied_bytes,
      max_heap_bytes);
  for (Allocation *allocation = allocations;
       allocation != NULL;
       allocation = allocation->next) {
    StellaValue object = allocation->object;
    const StellaObjectDescriptor *descriptor = object->descriptor;
    fprintf(
        output,
        "allocation=[%p,%p) object=%p descriptor=%s kind=%" PRIu32
        " managed=%zu primitive=%zu capacity=%zu gc_word=%" PRIuPTR "\n",
        (void *)object,
        (void *)((unsigned char *)object + allocation->rounded_bytes),
        (void *)object,
        descriptor->debug_name == NULL ? "(unnamed)" : descriptor->debug_name,
        descriptor->kind,
        descriptor->managed_count,
        descriptor->primitive_count,
        descriptor->slot_capacity,
        object->gc_word);
    for (size_t i = 0; i < descriptor->managed_count; ++i) {
      fprintf(
          output,
          "  managed[%zu]=%p\n",
          i,
          (void *)object->fields[i].managed);
    }
    for (size_t i = 0; i < descriptor->primitive_count; ++i) {
      fprintf(
          output,
          "  primitive[%zu]=%" PRIuPTR "\n",
          i,
          object->fields[descriptor->managed_count + i].primitive);
    }
  }
  stella_gc_print_roots(output);
}

