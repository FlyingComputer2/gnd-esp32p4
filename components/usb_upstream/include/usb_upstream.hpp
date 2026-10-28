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
#include <string>
// Service-loop profile since the previous call (time per section, counters).
std::string usb_upstream_profile();

#include <functional>
#include <optional>
// A local receive-only service, independently queued after application processing.
// Callbacks must be nonblocking; invoked under the USB service lock.
using UsbPacketSource = std::function<std::optional<bfc::shared_sized_buffer>()>;
esp_err_t usb_upstream_register_source(uint16_t channel, const char *name,
    UsbPacketSource receive, std::function<bool()> connected, const char *format,
    uint16_t mtu = 1445);

// A local bidirectional service: inbound DATA goes to send (false counts a
// drop), outbound DATA comes from receive. Both callbacks must be nonblocking;
// they are invoked under the USB service lock.
using UsbPacketSink = std::function<bool(const uint8_t *data, size_t len)>;
esp_err_t usb_upstream_register_service(uint16_t channel, const char *name,
    UsbPacketSource receive, UsbPacketSink send, std::function<bool()> connected,
    const char *format, uint16_t mtu = 1445);
