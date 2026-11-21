#include <stella/gc.h>

#include <stdlib.h>

static const StellaObjectDescriptor descriptor = {
    .abi_version = STELLA_C_GC_ABI_VERSION,
    .kind = STELLA_OBJECT_DATA,
    .managed_count = 1,
    .primitive_count = 1,
    .slot_capacity = 2,
    .debug_name = "c-abi-node",
};

int main(void) {
    StellaGcConfig config = {STELLA_C_GC_ABI_VERSION, 2048};
    stella_gc_init(&config);
    StellaValue root = NULL;
    StellaValue garbage = NULL;
    stella_gc_push_root(&root);
    stella_gc_push_root(&garbage);

    root = stella_gc_alloc(&descriptor);
    stella_object_init_managed(root, 0, root);
    stella_object_init_primitive(root, 0, 123456);

    for (size_t i = 0; i < 500; ++i) {
        garbage = stella_gc_alloc(&descriptor);
        stella_object_init_managed(garbage, 0, NULL);
        stella_object_init_primitive(garbage, 0, i);
    }

    if (stella_gc_read_managed(&root, 0) != root ||
        stella_object_read_primitive(root, 0) != 123456) {
        return 2;
    }

    stella_gc_pop_root(&garbage);
    stella_gc_pop_root(&root);
    stella_gc_shutdown();
    puts("ok");
    return EXIT_SUCCESS;
}
