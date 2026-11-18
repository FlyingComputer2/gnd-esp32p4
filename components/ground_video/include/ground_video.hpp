#pragma once
#include "endpoint/UpstreamQueue.h"
#include "esp_err.h"
#include <memory>
#include <cstdint>
esp_err_t ground_video_start(std::shared_ptr<winject::core::UpstreamQueue> upstream);
// Network-byte-order Ethernet address; zero disables multicast until DHCP returns.
void ground_video_set_ethernet(uint32_t address);
void ground_video_log_stats();

// The video upstream's l3 receive counters (winject::UpstreamRxStats of
// upstream 1), sampled by the caller. The air half (packets, gaps, bytes
// before l3's FEC) goes to the rover in the receiver's link reports; the FEC
// half is logged as GROUND_FEC.
struct ground_video_link_stats {
    uint64_t packets = 0;  // air slots received (FEC slots when FEC is on)
    uint64_t gaps = 0;     // slots missing from the LC sequence
    uint64_t bytes = 0;
    uint64_t fec_recovered = 0, fec_lost = 0, fec_gap = 0, fec_given_up = 0, fec_late = 0,
             fec_drops = 0;
    double fec_spread_ms = 0., fec_jitter_ms = 0.;
};
void ground_video_set_link_stats(const ground_video_link_stats &stats);
