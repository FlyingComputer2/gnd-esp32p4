#include "mplane_relay.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace mplane_relay
{
namespace
{

int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool parse_host_port(const std::string& s, sockaddr_in* out)
{
    const size_t colon = s.rfind(':');
    if (colon == std::string::npos)
    {
        return false;
    }
    const std::string host = s.substr(0, colon);
    char* end = nullptr;
    const long port = std::strtol(s.c_str() + colon + 1, &end, 10);
    if (*end != '\0' || port <= 0 || port > 65535)
    {
        return false;
    }
    std::memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(static_cast<uint16_t>(port));
    return inet_pton(AF_INET, host.c_str(), &out->sin_addr) == 1;
}

bool same_addr(const sockaddr_in& a, const sockaddr_in& b)
{
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

struct Client
{
    bool live = false;
    uint8_t id = 0;
    sockaddr_in addr {};
    int64_t last_ms = 0;
};

}  // namespace

int server_main(const ServerConfig& cfg)
{
    sockaddr_in bind_addr {};
    if (!cfg.send || !cfg.receive || cfg.max_clients == 0 || cfg.max_clients > 255 ||
        cfg.max_datagram < 2 || !parse_host_port(cfg.listen, &bind_addr))
    {
        return 1;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return 1;
    }
    if (bind(fd, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0)
    {
        close(fd);
        return 1;
    }
    std::vector<Client> clients(cfg.max_clients);
    // Ids roll over 0..255 so a reused slot gets a fresh id: a late reply for
    // a forgotten operator then finds no one instead of the newcomer.
    uint8_t next_id = 0;
    std::vector<uint8_t> air;
    std::vector<uint8_t> buf(cfg.max_datagram + 1);
    Stats st;

    auto client_for = [&](const sockaddr_in& from, int64_t now) -> Client*
    {
        Client* lru = nullptr;
        for (auto& c : clients)
        {
            if (c.live && same_addr(c.addr, from))
            {
                return &c;
            }
            if (!lru || !c.live || (lru->live && c.last_ms < lru->last_ms))
            {
                lru = &c;
            }
        }
        if (lru->live)
        {
            ++st.evicted;
        }
        lru->live = true;
        lru->addr = from;
        lru->id = next_id++;
        lru->last_ms = now;
        return lru;
    };

    while (!cfg.stop || !cfg.stop->load())
    {
        const int64_t now = now_ms();
        for (auto& c : clients)
        {
            if (c.live && now - c.last_ms > cfg.idle_ms)
            {
                c.live = false;
            }
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        timeval tv {0, cfg.poll_ms * 1000};
        if (select(fd + 1, &rd, nullptr, nullptr, &tv) > 0)
        {
            sockaddr_in from {};
            socklen_t len = sizeof(from);
            const ssize_t n = recvfrom(fd, buf.data() + 1, cfg.max_datagram, 0,
                                       reinterpret_cast<sockaddr*>(&from), &len);
            if (n > 0 && static_cast<size_t>(n) > cfg.max_datagram - 1)
            {
                ++st.too_big;
            }
            else if (n > 0)
            {
                Client* c = client_for(from, now);
                c->last_ms = now;
                buf[0] = c->id;
                if (cfg.send(buf.data(), static_cast<size_t>(n) + 1))
                {
                    ++st.requests;
                }
                else
                {
                    ++st.send_fail;
                }
            }
        }
        while (cfg.receive(&air))
        {
            if (air.size() < 2)
            {
                ++st.bad;
                continue;
            }
            const Client* owner = nullptr;
            for (const auto& c : clients)
            {
                if (c.live && c.id == air[0])
                {
                    owner = &c;
                    break;
                }
            }
            if (!owner)
            {
                ++st.no_client;
                continue;
            }
            if (sendto(fd, air.data() + 1, air.size() - 1, 0,
                       reinterpret_cast<const sockaddr*>(&owner->addr), sizeof(owner->addr)) < 0)
            {
                ++st.send_fail;
                continue;
            }
            ++st.replies;
        }
        if (cfg.on_stats)
        {
            cfg.on_stats(st);
        }
    }
    close(fd);
    return 0;
}

}  // namespace mplane_relay
