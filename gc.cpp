#include "internal/stella_abi.hpp"
#include "internal/gc_stats.hpp"

#include <algorithm>
#include <charconv>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Space {
    Space() = default;

    explicit Space(std::size_t bytes)

        : storage_(std::make_unique_for_overwrite<std::byte[]>(bytes)), capacity_(bytes) {
    }

    [[nodiscard]] std::byte *data() const noexcept {
        return storage_.get();
    }

    void clear() noexcept {
        used_ = 0;
        occupied_ = 0;
    }

    bool contains(StellaValue object) const noexcept {
        auto address = reinterpret_cast<std::uintptr_t>(object);
        auto start = reinterpret_cast<std::uintptr_t>(data());

        return address >= start && address - start < used_;
    }

  private:
    std::unique_ptr<std::byte[]> storage_;

  public:
    std::size_t capacity_{};
    std::size_t used_{};
    std::size_t occupied_{};
};

class Collector {
  public:
    void init(const StellaGcConfig *config);
    void shutdown();
    void pushRoot(StellaValue *root);
    void popRoot(StellaValue *root);
    void registerPermanentRoot(StellaValue *root);
    StellaValue alloc(const StellaObjectDescriptor *descriptor);
    void initManaged(StellaValue object, std::size_t index, StellaValue value);
    void initPrimitive(StellaValue object, std::size_t index, StellaPrimitive value);
    StellaValue readManaged(StellaValue *object_root, std::size_t index);
    void writeManaged(StellaValue *object_root, std::size_t index, StellaValue *value_root);
    StellaPrimitive readPrimitive(StellaValue object, std::size_t index);
    void writePrimitive(StellaValue object, std::size_t index, StellaPrimitive value);
    void updateObject(StellaValue *object_root,
        const StellaObjectDescriptor *new_descriptor,
        StellaValue *const *new_managed_roots,
        const StellaPrimitive *new_primitives);
    void printStatistics(FILE *output) const;
    void printRoots(FILE *output) const;
    void printState(FILE *output) const;

  private:
    enum class Lifecycle : std::uint8_t {
        Fresh,
        Active,
        Finished
    };
    enum class CollectionKind : std::uint8_t {
        Young,
        Full
    };

    Lifecycle lifecycle_{Lifecycle::Fresh};
    std::size_t max_heap_bytes_{};

    Space young_space_;
    Space old_space_;
    Space reserve_space_;

    static constexpr std::uintptr_t remembered_mark_ = 1;
    std::vector<StellaValue> remembered_;

    std::vector<StellaValue *> dynamic_roots_;
    std::vector<StellaValue *> permanent_roots_;
    GcStats counters_;

    void requireActive() const;
    static std::size_t objectSize(const StellaObjectDescriptor *descriptor);
    static std::size_t roundedSize(std::size_t size);
    static void validateDescriptor(const StellaObjectDescriptor *descriptor);
    bool inHeap(StellaValue object) const;
    void requireHeapObject(StellaValue object) const;
    void validateObject(StellaValue object) const;
    bool rememberObject(StellaValue object);
    void writeBarrier(StellaValue object, StellaValue value);
    StellaValue forward(StellaValue object, Space &destination, CollectionKind kind);
    void forwardRoots(Space &destination, CollectionKind kind);
    void forwardFields(StellaValue object, Space &destination, CollectionKind kind);
    void scanCopies(Space &destination, std::size_t scan, CollectionKind kind);
    void collectFull();
    void collectYoung();
    void refreshOccupancy();
    [[nodiscard]] bool allocationFits(
        const Space &space, std::size_t requested, std::size_t rounded) const;
    bool rootIsRegistered(StellaValue *root) const;
    void requireRegisteredRoot(StellaValue *root) const;
    [[nodiscard]] std::size_t youngCapacityFromEnvironment() const;
    static void printSpace(FILE *output, const char *name, const Space &space);
};

void Collector::requireActive() const {
    if (lifecycle_ != Lifecycle::Active) {
        stella_abi_violation("collector operation outside its active lifetime");
    }
}

std::size_t Collector::objectSize(const StellaObjectDescriptor *descriptor) {

    constexpr auto max_slots =
        (std::numeric_limits<std::size_t>::max() - sizeof(StellaObject)) / sizeof(StellaSlot);

    if (descriptor->slot_capacity > max_slots) {
        stella_abi_violation("object size overflows size_t");
    }

    std::size_t slots = descriptor->slot_capacity * sizeof(StellaSlot);

    return sizeof(StellaObject) + slots;
}

std::size_t Collector::roundedSize(std::size_t size) {
    std::size_t alignment = alignof(StellaObject);
    std::size_t remainder = size % alignment;

    if (remainder == 0) {
        return size;
    }

    std::size_t padding = alignment - remainder;

    if (size > std::numeric_limits<std::size_t>::max() - padding) {
        stella_abi_violation("aligned object size overflows size_t");
    }

    return size + padding;
}

void Collector::validateDescriptor(const StellaObjectDescriptor *descriptor) {
    if (descriptor == nullptr) {
        stella_abi_violation("null object descriptor");
    }

    if (descriptor->abi_version != STELLA_C_GC_ABI_VERSION) {
        stella_abi_violation("incompatible descriptor ABI version");
    }

    if (descriptor->kind < STELLA_OBJECT_DATA || descriptor->kind > STELLA_OBJECT_INDIRECTION) {
        stella_abi_violation("unknown object kind");
    }

    std::uint32_t known_flags = STELLA_DESCRIPTOR_STATIC | STELLA_DESCRIPTOR_UPDATABLE;

    if ((descriptor->flags & ~known_flags) != 0) {
        stella_abi_violation("unknown descriptor flag");
    }

    if ((descriptor->flags & STELLA_DESCRIPTOR_STATIC) != 0 &&
        (descriptor->flags & STELLA_DESCRIPTOR_UPDATABLE) != 0) {
        stella_abi_violation("static descriptor is marked updatable");
    }

    if (descriptor->managed_count > descriptor->slot_capacity ||
        descriptor->primitive_count > descriptor->slot_capacity - descriptor->managed_count) {
        stella_abi_violation("active fields exceed slot capacity");
    }

    if ((descriptor->kind == STELLA_OBJECT_CLOSURE || descriptor->kind == STELLA_OBJECT_THUNK) &&
        descriptor->entry_code == nullptr) {
        stella_abi_violation("executable descriptor has no entry code");
    }

    if ((descriptor->kind == STELLA_OBJECT_DATA || descriptor->kind == STELLA_OBJECT_INDIRECTION) &&
        descriptor->entry_code != nullptr) {
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

bool Collector::inHeap(StellaValue object) const {
    return young_space_.contains(object) || old_space_.contains(object);
}

void Collector::requireHeapObject(StellaValue object) const {
    if (object == nullptr) {
        stella_abi_violation("null object");
    }

    if (!inHeap(object)) {
        stella_abi_violation("object is not owned by the collector");
    }

    validateDescriptor(object->descriptor);
}

void Collector::validateObject(StellaValue object) const {
    if (object == nullptr) {
        stella_abi_violation("null object");
    }

    validateDescriptor(object->descriptor);

    if ((object->descriptor->flags & STELLA_DESCRIPTOR_STATIC) == 0 && !inHeap(object)) {
        stella_abi_violation("object is not owned by the collector");
    }
}

bool Collector::rememberObject(StellaValue object) {
    if (object->gc_word == remembered_mark_) {
        return false;
    }

    remembered_.push_back(object);
    object->gc_word = remembered_mark_;

    return true;
}

void Collector::writeBarrier(StellaValue object, StellaValue value) {

    if (old_space_.contains(object) && young_space_.contains(value) && rememberObject(object)) {
        ++counters_.write_barrier_activations_;
    }
}

StellaValue Collector::forward(StellaValue object, Space &destination, CollectionKind kind) {
    if (object == nullptr || (object->descriptor->flags & STELLA_DESCRIPTOR_STATIC) != 0) {
        return object;
    }

    requireHeapObject(object);

    if (kind == CollectionKind::Young && old_space_.contains(object)) {
        return object;
    }

    if (object->gc_word > remembered_mark_) {

        return reinterpret_cast<StellaValue>(object->gc_word);
    }

    const StellaObjectDescriptor *descriptor = object->descriptor;
    std::size_t requested = objectSize(descriptor);
    std::size_t rounded = roundedSize(requested);

    if (rounded > destination.capacity_ - destination.used_) {
        stella_abi_violation("copying exceeds destination capacity");
    }

    auto *copy = static_cast<StellaValue>(
        std::memcpy(destination.data() + destination.used_, object, requested));

    copy->fields =
        descriptor->slot_capacity == 0 ? nullptr : reinterpret_cast<StellaSlot *>(copy + 1);
    copy->gc_word = 0;

    std::size_t active = descriptor->managed_count + descriptor->primitive_count;

    for (std::size_t i = active; i < descriptor->slot_capacity; ++i) {
        copy->fields[i].primitive = 0;
    }

    destination.used_ += rounded;
    destination.occupied_ += requested;

    object->gc_word = reinterpret_cast<std::uintptr_t>(copy);

    return copy;
}

void Collector::forwardRoots(Space &destination, CollectionKind kind) {
    for (auto *root : dynamic_roots_) {
        *root = forward(*root, destination, kind);
    }

    for (auto *root : permanent_roots_) {
        *root = forward(*root, destination, kind);
    }
}

void Collector::forwardFields(StellaValue object, Space &destination, CollectionKind kind) {

    for (auto &field : std::span{object->fields, object->descriptor->managed_count}) {
        field.managed = forward(field.managed, destination, kind);
    }
}

void Collector::scanCopies(Space &destination, std::size_t scan, CollectionKind kind) {

    while (scan < destination.used_) {
        auto *object = reinterpret_cast<StellaValue>(destination.data() + scan);
        forwardFields(object, destination, kind);
        scan += roundedSize(objectSize(object->descriptor));
    }
}

void Collector::collectFull() {
    reserve_space_.clear();

    forwardRoots(reserve_space_, CollectionKind::Full);
    scanCopies(reserve_space_, 0, CollectionKind::Full);

    std::swap(old_space_, reserve_space_);
    reserve_space_.clear();
    young_space_.clear();

    remembered_.clear();
    ++counters_.collections_;
    ++counters_.full_collections_;
}

void Collector::collectYoung() {
    if (young_space_.used_ == 0) {
        return;
    }

    if (young_space_.used_ > old_space_.capacity_ - old_space_.used_) {
        collectFull();
        return;
    }

    std::size_t scan = old_space_.used_;
    forwardRoots(old_space_, CollectionKind::Young);

    for (auto *object : remembered_) {
        forwardFields(object, old_space_, CollectionKind::Young);
        object->gc_word = 0;
    }

    scanCopies(old_space_, scan, CollectionKind::Young);
    young_space_.clear();
    remembered_.clear();
    ++counters_.collections_;
    ++counters_.minor_collections_;
}

void Collector::refreshOccupancy() {
    counters_.young_occupied_bytes_ = young_space_.occupied_;
    counters_.old_occupied_bytes_ = old_space_.occupied_;
    counters_.occupied_bytes_ = young_space_.occupied_ + old_space_.occupied_;

    counters_.maximum_occupied_bytes_ =
        std::max(counters_.maximum_occupied_bytes_, counters_.occupied_bytes_);
    counters_.maximum_young_occupied_bytes_ =
        std::max(counters_.maximum_young_occupied_bytes_, young_space_.occupied_);
    counters_.maximum_old_occupied_bytes_ =
        std::max(counters_.maximum_old_occupied_bytes_, old_space_.occupied_);
}

bool Collector::allocationFits(
    const Space &space, std::size_t requested, std::size_t rounded) const {
    std::size_t occupied = young_space_.occupied_ + old_space_.occupied_;
    std::size_t used = young_space_.used_ + old_space_.used_;

    return requested <= max_heap_bytes_ - occupied && rounded <= max_heap_bytes_ - used &&
           rounded <= space.capacity_ - space.used_;
}

bool Collector::rootIsRegistered(StellaValue *root) const {
    return std::ranges::find(dynamic_roots_, root) != dynamic_roots_.end() ||
           std::ranges::find(permanent_roots_, root) != permanent_roots_.end();
}

void Collector::requireRegisteredRoot(StellaValue *root) const {
    if (root == nullptr || !rootIsRegistered(root)) {
        stella_abi_violation("field access uses an unregistered root");
    }
}

std::size_t Collector::youngCapacityFromEnvironment() const {
    const char *value = std::getenv("STELLA_GC_YOUNG_BYTES");

    if (value == nullptr) {
        return max_heap_bytes_ / 4 == 0 ? 1 : max_heap_bytes_ / 4;
    }

    std::string_view text{value};

    if (text.empty()) {
        stella_abi_violation("STELLA_GC_YOUNG_BYTES must be between 1 and max_heap_bytes");
    }

    std::size_t bytes = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), bytes);

    if (error != std::errc{} || end != text.data() + text.size()) {
        stella_abi_violation("STELLA_GC_YOUNG_BYTES must be a positive decimal byte count");
    }

    if (bytes == 0 || bytes > max_heap_bytes_) {
        stella_abi_violation("STELLA_GC_YOUNG_BYTES must be between 1 and max_heap_bytes");
    }

    return bytes;
}

void Collector::init(const StellaGcConfig *config) {
    if (lifecycle_ != Lifecycle::Fresh) {
        stella_abi_violation("collector initialised more than once");
    }

    if (config == nullptr) {
        stella_abi_violation("null collector configuration");
    }

    if (config->abi_version != STELLA_C_GC_ABI_VERSION) {
        stella_abi_violation("incompatible collector ABI version");
    }

    if (config->max_heap_bytes == 0) {
        stella_abi_violation("zero maximum heap size");
    }

    max_heap_bytes_ = config->max_heap_bytes;

    try {
        Space young{youngCapacityFromEnvironment()};
        Space old{max_heap_bytes_};
        Space reserve{max_heap_bytes_};

        young_space_ = std::move(young);
        old_space_ = std::move(old);
        reserve_space_ = std::move(reserve);
    } catch (const std::bad_alloc &) {
        stella_gc_out_of_memory(max_heap_bytes_);
    }

    lifecycle_ = Lifecycle::Active;
}

void Collector::shutdown() {
    requireActive();

    if (!dynamic_roots_.empty()) {
        stella_abi_violation("runtime shutdown with dynamic roots registered");
    }

    young_space_ = {};
    old_space_ = {};
    reserve_space_ = {};

    remembered_ = std::vector<StellaValue>{};
    dynamic_roots_ = std::vector<StellaValue *>{};
    permanent_roots_ = std::vector<StellaValue *>{};

    counters_.occupied_bytes_ = 0;
    counters_.young_occupied_bytes_ = 0;
    counters_.old_occupied_bytes_ = 0;
    lifecycle_ = Lifecycle::Finished;
}

void Collector::pushRoot(StellaValue *root) {
    requireActive();

    if (root == nullptr) {
        stella_abi_violation("null root slot");
    }

    if (rootIsRegistered(root)) {
        stella_abi_violation("duplicate root registration");
    }

    dynamic_roots_.push_back(root);
    counters_.dynamic_root_depth_ = dynamic_roots_.size();

    counters_.maximum_dynamic_root_depth_ =
        std::max(counters_.maximum_dynamic_root_depth_, dynamic_roots_.size());
}

void Collector::popRoot(StellaValue *root) {
    requireActive();

    if (dynamic_roots_.empty()) {
        stella_abi_violation("dynamic root stack underflow");
    }

    if (dynamic_roots_.back() != root) {
        stella_abi_violation("dynamic roots popped out of order");
    }

    dynamic_roots_.pop_back();
    counters_.dynamic_root_depth_ = dynamic_roots_.size();
}

void Collector::registerPermanentRoot(StellaValue *root) {
    requireActive();

    if (root == nullptr) {
        stella_abi_violation("null permanent-root slot");
    }

    if (rootIsRegistered(root)) {
        stella_abi_violation("duplicate root registration");
    }

    permanent_roots_.push_back(root);
    counters_.permanent_root_count_ = permanent_roots_.size();
}

StellaValue Collector::alloc(const StellaObjectDescriptor *descriptor) {
    requireActive();
    validateDescriptor(descriptor);

    if ((descriptor->flags & STELLA_DESCRIPTOR_STATIC) != 0) {
        stella_abi_violation("heap allocation requested with static descriptor");
    }

    std::size_t requested = objectSize(descriptor);
    std::size_t rounded = roundedSize(requested);

    bool large = rounded > young_space_.capacity_;

    if (!large && rounded > young_space_.capacity_ - young_space_.used_) {
        collectYoung();
    }

    Space &destination = large ? old_space_ : young_space_;

    if (!allocationFits(destination, requested, rounded)) {
        collectFull();
    }

    if (!allocationFits(destination, requested, rounded)) {
        ++counters_.failed_allocations_;
        stella_gc_out_of_memory(requested);
    }

    auto *object = reinterpret_cast<StellaValue>(destination.data() + destination.used_);
    std::construct_at(object,
        StellaObject{
            .descriptor = descriptor,
            .gc_word = 0,
            .fields = descriptor->slot_capacity == 0 ? nullptr
                                                     : reinterpret_cast<StellaSlot *>(object + 1),
        });

    for (std::size_t i = 0; i < descriptor->managed_count; ++i) {
        object->fields[i].managed = nullptr;
    }

    for (std::size_t i = descriptor->managed_count; i < descriptor->slot_capacity; ++i) {
        object->fields[i].primitive = 0;
    }

    destination.used_ += rounded;
    destination.occupied_ += requested;

    if (large && descriptor->managed_count != 0) {
        rememberObject(object);
    }

    counters_.requested_bytes_ += requested;
    counters_.rounded_bytes_ += rounded;
    ++counters_.allocated_objects_;
    refreshOccupancy();

    return object;
}

void Collector::initManaged(StellaValue object, std::size_t index, StellaValue value) {
    requireActive();
    requireHeapObject(object);

    if (index >= object->descriptor->managed_count) {
        stella_abi_violation("managed initialisation index out of range");
    }

    object->fields[index].managed = value;
}

void Collector::initPrimitive(StellaValue object, std::size_t index, StellaPrimitive value) {
    requireActive();
    requireHeapObject(object);

    if (index >= object->descriptor->primitive_count) {
        stella_abi_violation("primitive initialisation index out of range");
    }

    std::size_t physical = object->descriptor->managed_count + index;
    object->fields[physical].primitive = value;
}

StellaValue Collector::readManaged(StellaValue *object_root, std::size_t index) {
    requireActive();
    requireRegisteredRoot(object_root);
    validateObject(*object_root);

    if (index >= (*object_root)->descriptor->managed_count) {
        stella_abi_violation("managed read index out of range");
    }

    ++counters_.managed_reads_;

    return (*object_root)->fields[index].managed;
}

void Collector::writeManaged(StellaValue *object_root, std::size_t index, StellaValue *value_root) {
    requireActive();
    requireRegisteredRoot(object_root);
    requireRegisteredRoot(value_root);
    validateObject(*object_root);

    if (((*object_root)->descriptor->flags & STELLA_DESCRIPTOR_STATIC) != 0) {
        stella_abi_violation("managed write to static object");
    }

    if (index >= (*object_root)->descriptor->managed_count) {
        stella_abi_violation("managed write index out of range");
    }

    writeBarrier(*object_root, *value_root);
    (*object_root)->fields[index].managed = *value_root;
    ++counters_.managed_writes_;
}

StellaPrimitive Collector::readPrimitive(StellaValue object, std::size_t index) {
    requireActive();
    validateObject(object);

    if (index >= object->descriptor->primitive_count) {
        stella_abi_violation("primitive read index out of range");
    }

    ++counters_.primitive_reads_;

    return object->fields[object->descriptor->managed_count + index].primitive;
}

void Collector::writePrimitive(StellaValue object, std::size_t index, StellaPrimitive value) {
    requireActive();
    validateObject(object);

    if ((object->descriptor->flags & STELLA_DESCRIPTOR_STATIC) != 0) {
        stella_abi_violation("primitive write to static object");
    }

    if (index >= object->descriptor->primitive_count) {
        stella_abi_violation("primitive write index out of range");
    }

    object->fields[object->descriptor->managed_count + index].primitive = value;
    ++counters_.primitive_writes_;
}

void Collector::updateObject(StellaValue *object_root,
    const StellaObjectDescriptor *new_descriptor,
    StellaValue *const *new_managed_roots,
    const StellaPrimitive *new_primitives) {
    requireActive();
    requireRegisteredRoot(object_root);
    requireHeapObject(*object_root);
    const StellaObjectDescriptor *old_descriptor = (*object_root)->descriptor;
    validateDescriptor(new_descriptor);

    if ((old_descriptor->flags & STELLA_DESCRIPTOR_UPDATABLE) == 0 ||
        (new_descriptor->flags & STELLA_DESCRIPTOR_STATIC) != 0) {
        stella_abi_violation("incompatible object-layout update flags");
    }

    if (old_descriptor->slot_capacity != new_descriptor->slot_capacity) {
        stella_abi_violation("object-layout update changes slot capacity");
    }

    if (new_descriptor->managed_count != 0 && new_managed_roots == nullptr) {
        stella_abi_violation("missing managed values for object-layout update");
    }

    if (new_descriptor->primitive_count != 0 && new_primitives == nullptr) {
        stella_abi_violation("missing primitive values for object-layout update");
    }

    for (std::size_t i = 0; i < new_descriptor->managed_count; ++i) {
        requireRegisteredRoot(new_managed_roots[i]);
    }

    for (std::size_t i = 0; i < new_descriptor->managed_count; ++i) {
        writeBarrier(*object_root, *new_managed_roots[i]);
        (*object_root)->fields[i].managed = *new_managed_roots[i];
        ++counters_.managed_writes_;
    }

    for (std::size_t i = 0; i < new_descriptor->primitive_count; ++i) {
        std::size_t physical = new_descriptor->managed_count + i;
        (*object_root)->fields[physical].primitive = new_primitives[i];
        ++counters_.primitive_writes_;
    }

    std::size_t active = new_descriptor->managed_count + new_descriptor->primitive_count;

    for (std::size_t i = active; i < new_descriptor->slot_capacity; ++i) {
        (*object_root)->fields[i].primitive = 0;
    }

    (*object_root)->descriptor = new_descriptor;
}

void Collector::printStatistics(FILE *output) const {
    requireActive();

    if (output == nullptr) {
        stella_abi_violation("null statistics output stream");
    }

    std::fprintf(output, "requested bytes: %zu\n", counters_.requested_bytes_);
    std::fprintf(output, "rounded bytes: %zu\n", counters_.rounded_bytes_);
    std::fprintf(output, "allocated objects: %zu\n", counters_.allocated_objects_);

    std::fprintf(output, "collections: %zu\n", counters_.collections_);
    std::fprintf(output, "minor collections: %zu\n", counters_.minor_collections_);
    std::fprintf(output, "full collections: %zu\n", counters_.full_collections_);
    std::fprintf(output, "occupied bytes: %zu\n", counters_.occupied_bytes_);
    std::fprintf(output, "maximum occupied bytes: %zu\n", counters_.maximum_occupied_bytes_);
    std::fprintf(output, "young occupied bytes: %zu\n", counters_.young_occupied_bytes_);
    std::fprintf(
        output, "maximum young occupied bytes: %zu\n", counters_.maximum_young_occupied_bytes_);
    std::fprintf(output, "old occupied bytes: %zu\n", counters_.old_occupied_bytes_);
    std::fprintf(
        output, "maximum old occupied bytes: %zu\n", counters_.maximum_old_occupied_bytes_);

    std::fprintf(output, "managed reads: %zu\n", counters_.managed_reads_);
    std::fprintf(output, "managed writes: %zu\n", counters_.managed_writes_);
    std::fprintf(output, "primitive reads: %zu\n", counters_.primitive_reads_);
    std::fprintf(output, "primitive writes: %zu\n", counters_.primitive_writes_);

    std::fprintf(output, "read barrier activations: %zu\n", counters_.read_barrier_activations_);
    std::fprintf(output, "write barrier activations: %zu\n", counters_.write_barrier_activations_);

    std::fprintf(output, "dynamic roots: %zu\n", dynamic_roots_.size());
    std::fprintf(output, "maximum dynamic roots: %zu\n", counters_.maximum_dynamic_root_depth_);
    std::fprintf(output, "permanent roots: %zu\n", permanent_roots_.size());

    std::fprintf(output, "failed allocations: %zu\n", counters_.failed_allocations_);
}

void Collector::printRoots(FILE *output) const {
    requireActive();

    if (output == nullptr) {
        stella_abi_violation("null roots output stream");
    }

    for (std::size_t i = 0; i < dynamic_roots_.size(); ++i) {
        std::fprintf(output,
            "dynamic root %zu: slot=%p value=%p\n",
            i,
            static_cast<void *>(dynamic_roots_[i]),
            static_cast<void *>(*dynamic_roots_[i]));
    }

    for (std::size_t i = 0; i < permanent_roots_.size(); ++i) {
        std::fprintf(output,
            "permanent root %zu: slot=%p value=%p\n",
            i,
            static_cast<void *>(permanent_roots_[i]),
            static_cast<void *>(*permanent_roots_[i]));
    }
}

void Collector::printSpace(FILE *output, const char *name, const Space &space) {
    std::fprintf(output,
        "%s=[%p,%p) capacity=%zu used=%zu occupied=%zu free=%zu next=%p limit=%p\n",
        name,
        static_cast<void *>(space.data()),
        static_cast<void *>(space.data() + space.capacity_),
        space.capacity_,
        space.used_,
        space.occupied_,
        space.capacity_ - space.used_,
        static_cast<void *>(space.data() + space.used_),
        static_cast<void *>(space.data() + space.capacity_));

    for (std::size_t offset = 0; offset < space.used_;) {
        auto *object = reinterpret_cast<StellaValue>(space.data() + offset);
        const StellaObjectDescriptor *descriptor = object->descriptor;
        std::size_t rounded = roundedSize(objectSize(descriptor));
        std::fprintf(output,
            "allocation=[%p,%p) object=%p descriptor=%s kind=%" PRIu32
            " managed=%zu primitive=%zu capacity=%zu gc_word=%" PRIuPTR "\n",
            static_cast<void *>(object),
            static_cast<void *>(reinterpret_cast<std::byte *>(object) + rounded),
            static_cast<void *>(object),
            descriptor->debug_name == nullptr ? "(unnamed)" : descriptor->debug_name,
            descriptor->kind,
            descriptor->managed_count,
            descriptor->primitive_count,
            descriptor->slot_capacity,
            object->gc_word);

        for (std::size_t i = 0; i < descriptor->managed_count; ++i) {
            std::fprintf(
                output, "  managed[%zu]=%p\n", i, static_cast<void *>(object->fields[i].managed));
        }

        for (std::size_t i = 0; i < descriptor->primitive_count; ++i) {
            std::fprintf(output,
                "  primitive[%zu]=%" PRIuPTR "\n",
                i,
                object->fields[descriptor->managed_count + i].primitive);
        }

        std::size_t active = descriptor->managed_count + descriptor->primitive_count;

        for (std::size_t i = active; i < descriptor->slot_capacity; ++i) {
            std::fprintf(output, "  inactive[%zu]=%" PRIuPTR "\n", i, object->fields[i].primitive);
        }

        offset += rounded;
    }
}

void Collector::printState(FILE *output) const {
    requireActive();

    if (output == nullptr) {
        stella_abi_violation("null state output stream");
    }

    std::fprintf(output,
        "generational Cheney: occupied=%zu free=%zu limit=%zu promotion_age=1\n",
        counters_.occupied_bytes_,
        max_heap_bytes_ - counters_.occupied_bytes_,
        max_heap_bytes_);

    std::fprintf(output, "collection=idle scan=inactive\n");

    printSpace(output, "young", young_space_);
    printSpace(output, "old", old_space_);
    printSpace(output, "reserve", reserve_space_);
    std::fprintf(output, "remembered objects: %zu\n", remembered_.size());

    for (std::size_t i = 0; i < remembered_.size(); ++i) {
        std::fprintf(output, "  remembered[%zu]=%p\n", i, static_cast<void *>(remembered_[i]));
    }

    printRoots(output);
}

Collector &collector() {
    static Collector instance;
    return instance;
}

template <typename Action> decltype(auto) callAbi(Action &&action) noexcept {
    try {
        return std::forward<Action>(action)();
    } catch (const std::bad_alloc &) {
        stella_abi_violation("collector metadata allocation failed");
    } catch (const std::length_error &) {
        stella_abi_violation("collector metadata capacity exceeded");
    } catch (...) {
        stella_abi_violation("unexpected C++ exception in collector");
    }
}

}

extern "C" {

void stella_gc_init(const StellaGcConfig *config) {
    callAbi([&] { collector().init(config); });
}

void stella_gc_shutdown() {
    callAbi([&] { collector().shutdown(); });
}

void stella_gc_push_root(StellaValue *root) {
    callAbi([&] { collector().pushRoot(root); });
}

void stella_gc_pop_root(StellaValue *root) {
    callAbi([&] { collector().popRoot(root); });
}

void stella_gc_register_permanent_root(StellaValue *root) {
    callAbi([&] { collector().registerPermanentRoot(root); });
}

StellaValue stella_gc_alloc(const StellaObjectDescriptor *descriptor) {
    return callAbi([&] { return collector().alloc(descriptor); });
}

void stella_object_init_managed(StellaValue object, std::size_t index, StellaValue value) {
    callAbi([&] { collector().initManaged(object, index, value); });
}

void stella_object_init_primitive(StellaValue object, std::size_t index, StellaPrimitive value) {
    callAbi([&] { collector().initPrimitive(object, index, value); });
}

StellaValue stella_gc_read_managed(StellaValue *object_root, std::size_t index) {
    return callAbi([&] { return collector().readManaged(object_root, index); });
}

void stella_gc_write_managed(StellaValue *object_root, std::size_t index, StellaValue *value_root) {
    callAbi([&] { collector().writeManaged(object_root, index, value_root); });
}

StellaPrimitive stella_object_read_primitive(StellaValue object, std::size_t index) {
    return callAbi([&] { return collector().readPrimitive(object, index); });
}

void stella_object_write_primitive(StellaValue object, std::size_t index, StellaPrimitive value) {
    callAbi([&] { collector().writePrimitive(object, index, value); });
}

void stella_gc_update_object(StellaValue *object_root,
    const StellaObjectDescriptor *new_descriptor,
    StellaValue *const new_managed_roots[],
    const StellaPrimitive new_primitives[]) {
    callAbi([&] {
        collector().updateObject(object_root, new_descriptor, new_managed_roots, new_primitives);
    });
}

void stella_gc_print_statistics(FILE *output) {
    callAbi([&] { collector().printStatistics(output); });
}

void stella_gc_print_roots(FILE *output) {
    callAbi([&] { collector().printRoots(output); });
}

void stella_gc_print_state(FILE *output) {
    callAbi([&] { collector().printState(output); });
}
}
