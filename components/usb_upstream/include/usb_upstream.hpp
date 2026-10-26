#pragma once
#include "endpoint/UpstreamQueue.h"
#include "esp_err.h"
#include <cstdint>
#include <memory>

// Registers the L3-created app transport. Retained shared handle becomes
// disconnected on L3 teardown. No application queue pairs or forwarding loop.
esp_err_t usb_upstream_register(uint16_t channel, const char *name,
                                std::shared_ptr<winject::UpstreamQueue> upstream,
                                uint16_t mtu = 1445);
esp_err_t usb_upstream_start();
void usb_upstream_log_stats();
