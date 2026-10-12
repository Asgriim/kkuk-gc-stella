#pragma once

#include <cstddef>

struct GcStats {

    std::size_t requested_bytes_{};
    std::size_t rounded_bytes_{};
    std::size_t allocated_objects_{};

    std::size_t collections_{};
    std::size_t minor_collections_{};
    std::size_t full_collections_{};
    std::size_t occupied_bytes_{};
    std::size_t maximum_occupied_bytes_{};
    std::size_t young_occupied_bytes_{};
    std::size_t maximum_young_occupied_bytes_{};
    std::size_t old_occupied_bytes_{};
    std::size_t maximum_old_occupied_bytes_{};

    std::size_t managed_reads_{};
    std::size_t managed_writes_{};
    std::size_t primitive_reads_{};
    std::size_t primitive_writes_{};

    std::size_t read_barrier_activations_{};
    std::size_t write_barrier_activations_{};

    std::size_t dynamic_root_depth_{};
    std::size_t maximum_dynamic_root_depth_{};
    std::size_t permanent_root_count_{};

    std::size_t failed_allocations_{};
};
