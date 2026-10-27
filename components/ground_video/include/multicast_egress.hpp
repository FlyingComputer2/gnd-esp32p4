#pragma once
#include "video_fanout.hpp"
#include <cerrno>

namespace rover {
// UDP drivers can run out of TX descriptors during a recovery burst. Retain
// the head packet across a scheduler yield rather than turning pressure into
// an RTP gap. A retry never allows a later packet to overtake the head.
class MulticastEgress {
public:
    enum class Result { empty, sent, retry, failed };
    static constexpr unsigned max_retries = 64;
    template <class Send> Result step(VideoFanout &fanout, Send send) {
        if (!pending_) pending_ = fanout.pop(1);
        if (!pending_) return Result::empty;
        const int error = send(*pending_);
        if (!error) { reset(); return Result::sent; }
        if ((error == ENOMEM || error == ENOBUFS || error == EAGAIN || error == EWOULDBLOCK) &&
            ++retries_ < max_retries) return Result::retry;
        reset();
        return Result::failed;
    }
    bool reset() {
        const bool had_packet = bool(pending_);
        pending_.reset(); retries_ = 0;
        return had_packet;
    }
private:
    std::optional<bfc::shared_sized_buffer> pending_;
    unsigned retries_ = 0;
};
}
