#pragma once

/// @file core_allocator.hpp
/// @brief Adaptive Core Allocator for thread pinning.

#include <string>
#include <sched.h>

namespace fastbid {

class CoreAllocator {
public:
    struct ThreadAssignment {
        int core_id;
        int scheduling_policy; // e.g., SCHED_FIFO (1), SCHED_RR (2), SCHED_OTHER (0)
        int priority;
    };

    CoreAllocator();

    ThreadAssignment get_engine_assignment() const;
    ThreadAssignment get_io_assignment() const;

    int available_cores() const;
    const std::string& strategy_name() const;

    /// @brief Apply the assignment to the calling thread.
    static void apply_to_current_thread(const ThreadAssignment& assignment);

private:
    void detect_cores();
    void select_strategy();

    int num_cores_;
    int allowed_[CPU_SETSIZE]{};
    
    enum class Strategy { MINIMAL_2, STANDARD_4, FULL_8PLUS };
    Strategy strategy_;
    std::string strategy_name_;
};

} // namespace fastbid
