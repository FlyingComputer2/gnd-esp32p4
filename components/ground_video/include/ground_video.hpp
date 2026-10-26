#pragma once
#include "endpoint/UpstreamQueue.h"
#include "esp_err.h"
#include <memory>
#include <cstdint>
esp_err_t ground_video_start(std::shared_ptr<winject::UpstreamQueue> upstream);
// Network-byte-order Ethernet address; zero disables multicast until DHCP returns.
void ground_video_set_ethernet(uint32_t address);
void ground_video_log_stats();
