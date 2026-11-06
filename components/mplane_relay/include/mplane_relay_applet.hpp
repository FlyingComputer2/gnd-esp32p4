#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app_kit.hpp"
#include "mplane_relay.hpp"

namespace mplane_relay
{

struct AppletConfig
{
    std::string reactor = app_kit::k_apps_reactor;
    ServerConfig relay;
    uint32_t poll_ms = 2;
    std::string name = "relay";
};

struct Transport
{
    std::function<bool(const uint8_t* data, size_t len)> send;
    std::function<bool(std::vector<uint8_t>* out)> receive;
};

class Applet
{
public:
    Applet();
    ~Applet();
    Applet(const Applet&) = delete;
    Applet& operator=(const Applet&) = delete;
    bool configure(const AppletConfig& cfg, Transport transport);
    bool start();
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mplane_relay
