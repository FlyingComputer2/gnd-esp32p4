#include "Config.h"
#include <arpa/inet.h>
#include <cassert>
#include <cstdio>

int main()
{
    winject::Config cfg;
    cfg.domain = 0xb00b;
    cfg.modulation = "OFDM_24M";
    cfg.radio_device.mplane.sin_family = AF_INET;
    cfg.radio_device.mplane.sin_port = htons(22);
    cfg.radio_device.mplane.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    cfg.radio_device.dplane = cfg.radio_device.mplane;
    cfg.radio_device.dplane.sin_port = htons(9000);
    winject::UpstreamConfig video;
    video.index = 1;
    video.mode = winject::UpstreamMode::queue;
    video.bus_tx = 0xd4;
    video.bus_rx = 0xc3;
    cfg.upstreams.push_back(video);
    std::string error;
    assert(!cfg.validate(&error)); // Empty endpoints require an owner callback.
    cfg.on_upstream_queue = [](size_t, std::shared_ptr<winject::UpstreamQueue>) {};
    assert(cfg.validate(&error)); // Both validation passes must accept owned queues.
    cfg.upstreams[0].endpoint.rx = "partial";
    assert(!cfg.validate(&error));
    cfg.upstreams[0].endpoint.tx = "partial";
    cfg.upstreams[0].reactor = "reactor";
    assert(!cfg.validate(&error)); // Named endpoints still must be distinct.
    cfg.upstreams[0].endpoint.tx = "other";
    assert(cfg.validate(&error));
    puts("P4 startup: owned and named queue configuration validation passed");
}
