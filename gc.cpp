#include "internal/stella_abi.hpp"

#include "internal/gc_stats.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct Space {
    unsigned char *start;
    size_t used;
};

enum Lifecycle {
    LIFECYCLE_FRESH,
    LIFECYCLE_ACTIVE,
    LIFECYCLE_FINISHED
};

static enum Lifecycle lifecycle = LIFECYCLE_FRESH;
static size_t max_heap_bytes;

static Space from_space;
static Space to_space;

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
    size_t alignment = alignof(StellaObject);
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

    if (descriptor->kind < STELLA_OBJECT_DATA || descriptor->kind > STELLA_OBJECT_INDIRECTION) {
        stella_abi_violation("unknown object kind");
    }

    uint32_t known_flags = STELLA_DESCRIPTOR_STATIC | STELLA_DESCRIPTOR_UPDATABLE;

    if ((descriptor->flags & ~known_flags) != 0) {
        stella_abi_violation("unknown descriptor flag");
    }

    if ((descriptor->flags & STELLA_DESCRIPTOR_STATIC) != 0 &&
        (descriptor->flags & STELLA_DESCRIPTOR_UPDATABLE) != 0) {
        stella_abi_violation("static descriptor is marked updatable");
    }

    if (add_overflows(descriptor->managed_count, descriptor->primitive_count)) {
        stella_abi_violation("active field count overflows size_t");
    }

    if (descriptor->managed_count + descriptor->primitive_count > descriptor->slot_capacity) {
        stella_abi_violation("active fields exceed slot capacity");
    }

    if ((descriptor->kind == STELLA_OBJECT_CLOSURE || descriptor->kind == STELLA_OBJECT_THUNK) &&
        descriptor->entry_code == NULL) {
        stella_abi_violation("executable descriptor has no entry code");
    }

    if ((descriptor->kind == STELLA_OBJECT_DATA || descriptor->kind == STELLA_OBJECT_INDIRECTION) &&
        descriptor->entry_code != NULL) {
        stella_abi_violation("non-executable descriptor has entry code");
    }

    if (descriptor->kind == STELLA_OBJECT_THUNK && descriptor->entry_arity != 0) {
        stella_abi_violation("thunk descriptor has non-zero entry arity");
    }

    if ((descriptor->kind == STELLA_OBJECT_DATA || descriptor->kind == STELLA_OBJECT_INDIRECTION) &&
        descriptor->entry_arity != 0) {
        stella_abi_violation("non-executable descriptor has non-zero entry arity");
    }
}

static bool in_from_space(StellaValue object) {
    uintptr_t address = reinterpret_cast<uintptr_t>(object);
    uintptr_t start = reinterpret_cast<uintptr_t>(from_space.start);

    return address >= start && address - start < from_space.used;
}

static void require_heap_object(StellaValue object) {
    if (object == NULL) {
        stella_abi_violation("null object");
    }

    if (!in_from_space(object)) {
        stella_abi_violation("object is not owned by the collector");
    }

    validate_descriptor(object->descriptor);
}

static void validate_object(StellaValue object) {
    if (object == NULL) {
        stella_abi_violation("null object");
    }

    validate_descriptor(object->descriptor);

    if ((object->descriptor->flags & STELLA_DESCRIPTOR_STATIC) == 0 && !in_from_space(object)) {
        stella_abi_violation("object is not owned by the collector");
    }
}

static StellaValue forward(StellaValue object) {
    if (object == NULL || (object->descriptor->flags & STELLA_DESCRIPTOR_STATIC) != 0) {
        return object;
    }

    require_heap_object(object);

    if (object->gc_word != 0) {
        return reinterpret_cast<StellaValue>(object->gc_word);
    }

    const StellaObjectDescriptor *descriptor = object->descriptor;
    size_t requested = object_size(descriptor);
    size_t rounded = rounded_size(requested);

    if (rounded > max_heap_bytes - to_space.used) {
        stella_abi_violation("copying exceeds semispace capacity");
    }

    StellaValue copy =
        static_cast<StellaValue>(memcpy(to_space.start + to_space.used, object, requested));

    copy->fields = descriptor->slot_capacity == 0 ? NULL : reinterpret_cast<StellaSlot *>(copy + 1);

    copy->gc_word = 0;

    size_t active = descriptor->managed_count + descriptor->primitive_count;

    for (size_t i = active; i < descriptor->slot_capacity; ++i) {
        copy->fields[i].primitive = 0;
    }

    to_space.used += rounded;

    object->gc_word = reinterpret_cast<uintptr_t>(copy);

    return copy;
}

static void collect(void) {
    to_space.used = 0;

    for (size_t i = 0; i < dynamic_root_depth; ++i) {
        *dynamic_roots[i] = forward(*dynamic_roots[i]);
    }

    for (size_t i = 0; i < permanent_root_count; ++i) {
        *permanent_roots[i] = forward(*permanent_roots[i]);
    }

    size_t scan = 0;
    size_t live_bytes = 0;

    while (scan < to_space.used) {
        StellaValue object = reinterpret_cast<StellaValue>(to_space.start + scan);

        for (size_t i = 0; i < object->descriptor->managed_count; ++i) {
            object->fields[i].managed = forward(object->fields[i].managed);
        }

        size_t requested = object_size(object->descriptor);
        live_bytes += requested;
        scan += rounded_size(requested);
    }

    Space old_space = from_space;
    from_space = to_space;
    to_space = old_space;
    to_space.used = 0;

    counters.occupied_bytes = live_bytes;
    ++counters.collections;
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

static void grow_roots(StellaValue ***roots, size_t *capacity, size_t required) {
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

    *roots = static_cast<StellaValue **>(storage);
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

    from_space.start = static_cast<unsigned char *>(malloc(max_heap_bytes));
    to_space.start = static_cast<unsigned char *>(malloc(max_heap_bytes));

    if (from_space.start == NULL || to_space.start == NULL) {
        free(from_space.start);
        free(to_space.start);
        stella_gc_out_of_memory(max_heap_bytes);
    }

    lifecycle = LIFECYCLE_ACTIVE;
}

void stella_gc_shutdown(void) {
    require_active();

    if (dynamic_root_depth != 0) {
        stella_abi_violation("runtime shutdown with dynamic roots registered");
    }

    free(from_space.start);
    free(to_space.start);
    from_space = {};
    to_space = {};

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

    grow_roots(&permanent_roots, &permanent_root_capacity, permanent_root_count + 1);
    permanent_roots[permanent_root_count++] = root;
    counters.permanent_root_count = permanent_root_count;
}

StellaValue stella_gc_alloc(const StellaObjectDescriptor *descriptor) {
    require_active();
    validate_descriptor(descriptor);

    if ((descriptor->flags & STELLA_DESCRIPTOR_STATIC) != 0) {
        stella_abi_violation("heap allocation requested with static descriptor");
    }

    size_t requested = object_size(descriptor);
    size_t rounded = rounded_size(requested);

    if (requested > max_heap_bytes - counters.occupied_bytes ||
        rounded > max_heap_bytes - from_space.used) {
        collect();
    }

    if (requested > max_heap_bytes - counters.occupied_bytes ||
        rounded > max_heap_bytes - from_space.used) {
        ++counters.failed_allocations;
        stella_gc_out_of_memory(requested);
    }

    StellaValue object = reinterpret_cast<StellaValue>(from_space.start + from_space.used);
    object->descriptor = descriptor;
    object->gc_word = 0;
    object->fields = descriptor->slot_capacity == 0 ? NULL : (StellaSlot *)(object + 1);

    for (size_t i = 0; i < descriptor->managed_count; ++i) {
        object->fields[i].managed = NULL;
    }

    for (size_t i = descriptor->managed_count; i < descriptor->slot_capacity; ++i) {
        object->fields[i].primitive = 0;
    }

    from_space.used += rounded;

    counters.requested_bytes += requested;
    counters.rounded_bytes += rounded;
    ++counters.allocated_objects;
    counters.occupied_bytes += requested;

    if (counters.occupied_bytes > counters.maximum_occupied_bytes) {
        counters.maximum_occupied_bytes = counters.occupied_bytes;
    }

    return object;
}

void stella_object_init_managed(StellaValue object, size_t index, StellaValue value) {
    require_active();
    require_heap_object(object);

    if (index >= object->descriptor->managed_count) {
        stella_abi_violation("managed initialisation index out of range");
    }

    object->fields[index].managed = value;
}

void stella_object_init_primitive(StellaValue object, size_t index, StellaPrimitive value) {
    require_active();
    require_heap_object(object);

    if (index >= object->descriptor->primitive_count) {
        stella_abi_violation("primitive initialisation index out of range");
    }

    size_t physical = object->descriptor->managed_count + index;
    object->fields[physical].primitive = value;
}

StellaValue stella_gc_read_managed(StellaValue *object_root, size_t index) {
    require_active();
    require_registered_root(object_root);
    validate_object(*object_root);

    if (index >= (*object_root)->descriptor->managed_count) {
        stella_abi_violation("managed read index out of range");
    }

    ++counters.managed_reads;

    return (*object_root)->fields[index].managed;
}

void stella_gc_write_managed(StellaValue *object_root, size_t index, StellaValue *value_root) {
    require_active();
    require_registered_root(object_root);
    require_registered_root(value_root);
    validate_object(*object_root);

    if (((*object_root)->descriptor->flags & STELLA_DESCRIPTOR_STATIC) != 0) {
        stella_abi_violation("managed write to static object");
    }

    if (index >= (*object_root)->descriptor->managed_count) {
        stella_abi_violation("managed write index out of range");
    }

    (*object_root)->fields[index].managed = *value_root;
    ++counters.managed_writes;
}

StellaPrimitive stella_object_read_primitive(StellaValue object, size_t index) {
    require_active();
    validate_object(object);

    if (index >= object->descriptor->primitive_count) {
        stella_abi_violation("primitive read index out of range");
    }

    ++counters.primitive_reads;

    return object->fields[object->descriptor->managed_count + index].primitive;
}

void stella_object_write_primitive(StellaValue object, size_t index, StellaPrimitive value) {
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

void stella_gc_update_object(StellaValue *object_root,
    const StellaObjectDescriptor *new_descriptor,
    StellaValue *const new_managed_roots[],
    const StellaPrimitive new_primitives[]) {
    require_active();
    require_registered_root(object_root);
    require_heap_object(*object_root);
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

    for (size_t i = 0; i < new_descriptor->managed_count; ++i) {
        (*object_root)->fields[i].managed = *new_managed_roots[i];
        ++counters.managed_writes;
    }

    for (size_t i = 0; i < new_descriptor->primitive_count; ++i) {
        size_t physical = new_descriptor->managed_count + i;
        (*object_root)->fields[physical].primitive = new_primitives[i];
        ++counters.primitive_writes;
    }

    size_t active = new_descriptor->managed_count + new_descriptor->primitive_count;

    for (size_t i = active; i < new_descriptor->slot_capacity; ++i) {
        (*object_root)->fields[i].primitive = 0;
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
    fprintf(output, "maximum occupied bytes: %zu\n", counters.maximum_occupied_bytes);

    fprintf(output, "managed reads: %zu\n", counters.managed_reads);
    fprintf(output, "managed writes: %zu\n", counters.managed_writes);
    fprintf(output, "primitive reads: %zu\n", counters.primitive_reads);
    fprintf(output, "primitive writes: %zu\n", counters.primitive_writes);

    fprintf(output, "read barrier activations: %zu\n", counters.read_barrier_activations);
    fprintf(output, "write barrier activations: %zu\n", counters.write_barrier_activations);

    fprintf(output, "dynamic roots: %zu\n", dynamic_root_depth);
    fprintf(output, "maximum dynamic roots: %zu\n", counters.maximum_dynamic_root_depth);
    fprintf(output, "permanent roots: %zu\n", permanent_root_count);

    fprintf(output, "failed allocations: %zu\n", counters.failed_allocations);
}

void stella_gc_print_roots(FILE *output) {
    require_active();

    if (output == NULL) {
        stella_abi_violation("null roots output stream");
    }

    for (size_t i = 0; i < dynamic_root_depth; ++i) {
        fprintf(output,
            "dynamic root %zu: slot=%p value=%p\n",
            i,
            (void *)dynamic_roots[i],
            (void *)*dynamic_roots[i]);
    }

    for (size_t i = 0; i < permanent_root_count; ++i) {
        fprintf(output,
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

    fprintf(output,
        "%s: occupied=%zu free=%zu limit=%zu\n",
        "Cheney",
        counters.occupied_bytes,
        max_heap_bytes - counters.occupied_bytes,
        max_heap_bytes);

    fprintf(output,
        "active=[%p,%p) capacity=%zu used=%zu free=%zu next=%p\n",
        (void *)from_space.start,
        (void *)(from_space.start + max_heap_bytes),
        max_heap_bytes,
        from_space.used,
        max_heap_bytes - from_space.used,
        (void *)(from_space.start + from_space.used));

    fprintf(output,
        "reserve=[%p,%p) capacity=%zu used=0 free=%zu\n",
        (void *)to_space.start,
        (void *)(to_space.start + max_heap_bytes),
        max_heap_bytes,
        max_heap_bytes);

    for (size_t offset = 0; offset < from_space.used;) {
        StellaValue object = reinterpret_cast<StellaValue>(from_space.start + offset);
        const StellaObjectDescriptor *descriptor = object->descriptor;
        size_t rounded = rounded_size(object_size(descriptor));
        fprintf(output,
            "allocation=[%p,%p) object=%p descriptor=%s kind=%" PRIu32
            " managed=%zu primitive=%zu capacity=%zu gc_word=%" PRIuPTR "\n",
            (void *)object,
            (void *)((unsigned char *)object + rounded),
            (void *)object,
            descriptor->debug_name == NULL ? "(unnamed)" : descriptor->debug_name,
            descriptor->kind,
            descriptor->managed_count,
            descriptor->primitive_count,
            descriptor->slot_capacity,
            object->gc_word);

        for (size_t i = 0; i < descriptor->managed_count; ++i) {
            fprintf(output, "  managed[%zu]=%p\n", i, (void *)object->fields[i].managed);
        }

        for (size_t i = 0; i < descriptor->primitive_count; ++i) {
            fprintf(output,
                "  primitive[%zu]=%" PRIuPTR "\n",
                i,
                object->fields[descriptor->managed_count + i].primitive);
        }

        offset += rounded;
    }

    stella_gc_print_roots(output);
}
