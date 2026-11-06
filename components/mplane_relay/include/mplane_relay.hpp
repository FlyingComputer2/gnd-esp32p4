#pragma once

// Ground half of the multi-client m-plane relay. The host watches fd(), calls
// on_readable() for operator datagrams, feeds air replies to on_air(), and
// calls tick() for idle expiry. The core is portable and has no thread.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mplane_relay
{

struct Stats
{
    uint64_t requests = 0;
    uint64_t replies = 0;
    uint64_t no_client = 0;
    uint64_t bad = 0;
    uint64_t send_fail = 0;
    uint64_t too_big = 0;
    uint64_t evicted = 0;
};

struct ServerConfig
{
    std::string listen = "0.0.0.0:5102";
    size_t max_clients = 8;
    int64_t idle_ms = 120000;
    size_t max_datagram = 1445;
};

struct Hooks
{
    std::function<bool(const uint8_t* data, size_t len)> send_air;
    std::function<int64_t()> now_ms;
};

class Server
{
public:
    Server(ServerConfig cfg, Hooks hooks);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool ok() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    void on_readable();
    void on_air(const uint8_t* data, size_t len);
    void tick();
    const Stats& stats() const { return stats_; }

private:
    struct Client
    {
        bool live = false;
        uint8_t id = 0;
        std::vector<uint8_t> addr;
        int64_t last_ms = 0;
    };
    Client* client_for(const void* addr, int64_t now);

    ServerConfig cfg_;
    Hooks hooks_;
    int fd_ = -1;
    uint8_t next_id_ = 0;
    std::vector<Client> clients_;
    std::vector<uint8_t> buf_;
    Stats stats_;
};

}  // namespace mplane_relay
