// Owned applets on a named reactor. Each applet has only the
// init/start/stop lifecycle and never owns a main loop.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "app_kit.hpp"
#include "endpoint/UpstreamQueue.h"
#include "synch/Synch.h"

namespace applets
{

struct bwtest_config
{
    std::string reactor = app_kit::k_apps_reactor;
    std::string console = "0.0.0.0:5100";
    size_t payload_bytes = 1400;
    uint32_t poll_ms = 2;
    app_kit::thread_spec sender {"bwtest", 6 * 1024, 5, -1, app_kit::stack_in::psram, false};
};

class BwtestApplet
{
public:
    BwtestApplet();
    ~BwtestApplet();
    BwtestApplet(const BwtestApplet&) = delete;
    BwtestApplet& operator=(const BwtestApplet&) = delete;
    bool configure(std::shared_ptr<winject::UpstreamQueue> up, const bwtest_config& cfg);
    bool start();
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct synch_server_config
{
    std::string reactor = app_kit::k_apps_reactor;
    std::string console = "0.0.0.0:5110";
    krono::EdgeSource edges;
    uint32_t poll_ms = 1;
};

// The nonblocking server and its console both run on the reactor, so request
// timestamping is serialized with the queue drain and needs no worker thread.
class SynchServerApplet
{
public:
    SynchServerApplet();
    ~SynchServerApplet();
    SynchServerApplet(const SynchServerApplet&) = delete;
    SynchServerApplet& operator=(const SynchServerApplet&) = delete;
    bool configure(krono::Transport transport, const synch_server_config& cfg);
    bool start();
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace applets
