// Host test for components/mplane_relay: two operators on one relay port each
// get only their own replies; a fake rover answers "<request>#<id>".

#include "mplane_relay.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr uint16_t k_port = 15102;

struct Operator
{
    int fd = -1;
    sockaddr_in relay {};

    Operator()
    {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        timeval tv {0, 300000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        relay.sin_family = AF_INET;
        relay.sin_port = htons(k_port);
        relay.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    ~Operator() { close(fd); }
    void send(const std::string& s)
    {
        sendto(fd, s.data(), s.size(), 0, reinterpret_cast<sockaddr*>(&relay), sizeof(relay));
    }
    std::string recv()
    {
        char buf[2048];
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
    }
};

std::string id_of(const std::string& reply)
{
    return reply.substr(reply.find('#') + 1);
}

}  // namespace

int main()
{
    std::mutex mu;
    std::deque<std::vector<uint8_t>> up;  // rover -> ground
    std::atomic<bool> stop {false};
    std::atomic<bool> rover_on {true};
    mplane_relay::ServerConfig cfg;
    cfg.listen = "127.0.0.1:" + std::to_string(k_port);
    cfg.max_clients = 2;
    mplane_relay::Hooks hooks;
    // Fake rover: answers every request at once, tagged with its id.
    hooks.send_air = [&](const uint8_t* d, size_t n) {
        if (!rover_on)
        {
            return true;
        }
        std::string reply(d + 1, d + n);
        reply += "#" + std::to_string(d[0]);
        std::vector<uint8_t> air {d[0]};
        air.insert(air.end(), reply.begin(), reply.end());
        std::lock_guard<std::mutex> l(mu);
        up.push_back(air);
        return true;
    };
    hooks.now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    mplane_relay::Server server(cfg, hooks);
    assert(server.ok());
    std::thread relay([&] {
        while (!stop.load())
        {
            pollfd pfd {server.fd(), POLLIN, 0};
            if (poll(&pfd, 1, 5) > 0)
                server.on_readable();
            for (;;)
            {
                std::vector<uint8_t> air;
                {
                    std::lock_guard<std::mutex> l(mu);
                    if (up.empty()) break;
                    air = std::move(up.front());
                    up.pop_front();
                }
                server.on_air(air.data(), air.size());
            }
            server.tick();
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    Operator a, b, c;
    a.send("stats");
    const std::string ra = a.recv();
    assert(ra.rfind("stats#", 0) == 0);
    b.send("status");
    const std::string rb = b.recv();
    assert(rb.rfind("status#", 0) == 0);
    assert(id_of(ra) != id_of(rb));

    // Interleaved: replies are not crossed.
    a.send("clr");
    b.send("mode rtt");
    assert(a.recv() == "clr#" + id_of(ra));
    assert(b.recv() == "mode rtt#" + id_of(rb));

    // A request too big for one air datagram is dropped.
    a.send(std::string(1445, 'x'));
    assert(a.recv().empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // A third operator replaces the least recently active (b), with a new id.
    c.send("stats");
    const std::string rc = c.recv();
    assert(id_of(rc) != id_of(ra) && id_of(rc) != id_of(rb));

    // A late reply for b's forgotten id reaches no one.
    {
        std::lock_guard<std::mutex> l(mu);
        std::vector<uint8_t> late {static_cast<uint8_t>(std::stoi(id_of(rb))), 'z'};
        up.push_back(late);
    }
    assert(b.recv().empty());

    stop = true;
    relay.join();
    const auto last = server.stats();
    assert(last.requests == 5);
    assert(last.replies == 5);
    assert(last.too_big == 1);
    assert(last.evicted == 1);
    assert(last.no_client == 1);
    std::printf("mplane_relay_test: ok\n");
    return 0;
}
