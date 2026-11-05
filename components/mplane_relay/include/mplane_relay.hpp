#pragma once

// Ground half of a multi-client m-plane relay over an L3-owned queue upstream.
//
// Operators send console requests (one per datagram) to a UDP port here. Each
// distinct sender address gets a 1-byte client id, and requests go over the
// air as
//   [client_id u8][request bytes]
// The rover half (rov-esp32p4 components/mplane_relay) sends each request to
// its local console from that client's own loopback socket and returns the
// reply with the same id; this half sends it to the operator who owns the id.
// Several operators can use one console at once and see only their own
// replies, unlike an L3 udp_server upstream, which replies to the last sender.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mplane_relay
{

struct Stats
{
    uint64_t requests = 0;   // operator datagrams sent over the air
    uint64_t replies = 0;    // air replies sent to their operator
    uint64_t no_client = 0;  // air replies whose id has no live operator
    uint64_t bad = 0;        // empty air replies
    uint64_t send_fail = 0;  // air send rejected, or sendto failed
    uint64_t too_big = 0;    // operator requests over max_datagram - 1 bytes
    uint64_t evicted = 0;    // operators forgotten to make room
};

struct ServerConfig
{
    // L3 queue: send one air datagram (false on reject); receive one, non-blocking.
    std::function<bool(const uint8_t* data, size_t len)> send;
    std::function<bool(std::vector<uint8_t>* out)> receive;
    // Operator socket, host:port.
    std::string listen = "0.0.0.0:5102";
    // Operators remembered at most; the least recently active is forgotten
    // for a new one. The rover keeps fewer sockets (4) and closes its least
    // recently used; that only costs a reply still in flight, since the next
    // request opens a new socket (the consoles keep no per-client state).
    size_t max_clients = 8;
    // An operator silent this long is forgotten.
    int idle_ms = 120000;
    // select() timeout between drains of the air queue.
    int poll_ms = 5;
    // Largest air datagram (L3 stream payload limit).
    size_t max_datagram = 1445;
    // Set to stop server_main; nullptr = run forever.
    const std::atomic<bool>* stop = nullptr;
    // Optional: receives counters after every loop pass.
    std::function<void(const Stats&)> on_stats;
};

// Runs the relay until *stop. Returns 0, or 1 when the config or the socket
// is unusable.
int server_main(const ServerConfig& cfg);

}  // namespace mplane_relay
