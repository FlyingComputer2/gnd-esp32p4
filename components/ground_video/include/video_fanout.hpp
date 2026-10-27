#pragma once
#include <bfc/shared_sized_buffer.hpp>
#include <array>
#include <deque>
#include <mutex>
#include <optional>
#include <cstdint>

namespace rover {
// Each consumer owns a bounded queue; eviction in one cannot consume the other.
class VideoFanout {
public:
    // USB credits can pause delivery across codec/rate transitions. Keep
    // a bounded burst reserve without growing the Ethernet branch.
    static constexpr size_t capacity = 512;
    // The recovery loop can emit 64 packets in one scheduling turn. Ethernet
    // must absorb that burst even when its lower-priority task has not run yet.
    static constexpr size_t multicast_capacity = 128;
    void push(const bfc::shared_sized_buffer &packet) {
        std::lock_guard<std::mutex> lock(mu_);
        for (size_t i = 0; i < queues_.size(); ++i) {
            auto &q = queues_[i];
            const size_t limit = i == 0 ? capacity : multicast_capacity;
            if (q.size() == limit) { q.pop_front(); ++drops_[i]; }
            q.push_back(packet);
        }
    }
    std::optional<bfc::shared_sized_buffer> pop(size_t consumer) {
        std::lock_guard<std::mutex> lock(mu_);
        auto &q = queues_.at(consumer);
        if (q.empty()) return std::nullopt;
        auto packet = std::move(q.front()); q.pop_front(); return packet;
    }
    uint64_t drops(size_t consumer) const {
        std::lock_guard<std::mutex> lock(mu_); return drops_.at(consumer);
    }
private:
    mutable std::mutex mu_;
    std::array<std::deque<bfc::shared_sized_buffer>, 2> queues_;
    std::array<uint64_t, 2> drops_{};
};
}
