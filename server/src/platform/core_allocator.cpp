#include "platform/core_allocator.hpp"
#include <cstdio>
#include <pthread.h>
#include <sched.h>
namespace fastbid {
CoreAllocator::CoreAllocator() { detect_cores(); select_strategy(); }
void CoreAllocator::detect_cores() {
    cpu_set_t set; CPU_ZERO(&set);
    num_cores_ = 0;
    if (sched_getaffinity(0, sizeof(set), &set) == 0)
        for (int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &set)) allowed_[num_cores_++] = i;
    if (!num_cores_) { allowed_[0] = 0; num_cores_ = 1; }
}
void CoreAllocator::select_strategy() { strategy_name_ = "AFFINITY_MASK_NORMAL_SCHEDULER"; }
CoreAllocator::ThreadAssignment CoreAllocator::get_engine_assignment() const { return {allowed_[num_cores_ - 1], SCHED_OTHER, 0}; }
CoreAllocator::ThreadAssignment CoreAllocator::get_io_assignment() const { return {allowed_[0], SCHED_OTHER, 0}; }
int CoreAllocator::available_cores() const { return num_cores_; }
const std::string& CoreAllocator::strategy_name() const { return strategy_name_; }
void CoreAllocator::apply_to_current_thread(const ThreadAssignment& assignment) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(assignment.core_id, &set);
    int err = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (err) std::fprintf(stderr, "[CPU] Affinity failed: %d\n", err);
    else std::printf("[CPU] Pinned to allowed CPU %d, normal scheduling\n", assignment.core_id);
}
}
