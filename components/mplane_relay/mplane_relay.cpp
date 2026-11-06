#include "mplane_relay.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <utility>

namespace mplane_relay
{
namespace
{

bool parse_host_port(const std::string& s, sockaddr_in* out)
{
    const size_t colon = s.rfind(':');
    if (colon == std::string::npos)
    {
        return false;
    }
    char* end = nullptr;
    const long port = std::strtol(s.c_str() + colon + 1, &end, 10);
    if (*end != '\0' || port <= 0 || port > 65535)
    {
        return false;
    }
    std::memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(static_cast<uint16_t>(port));
    return inet_pton(AF_INET, s.substr(0, colon).c_str(), &out->sin_addr) == 1;
}

bool same_addr(const std::vector<uint8_t>& bytes, const sockaddr_in& addr)
{
    if (bytes.size() != sizeof(addr))
    {
        return false;
    }
    sockaddr_in saved {};
    std::memcpy(&saved, bytes.data(), sizeof(saved));
    return saved.sin_addr.s_addr == addr.sin_addr.s_addr && saved.sin_port == addr.sin_port;
}

}  // namespace

Server::Server(ServerConfig cfg, Hooks hooks)
    : cfg_(std::move(cfg)), hooks_(std::move(hooks))
{
    sockaddr_in bind_addr {};
    if (!hooks_.send_air || !hooks_.now_ms || cfg_.max_clients == 0 || cfg_.max_clients > 255 ||
        cfg_.max_datagram < 2 || !parse_host_port(cfg_.listen, &bind_addr))
    {
        return;
    }
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0 || bind(fd_, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0)
    {
        if (fd_ >= 0)
        {
            close(fd_);
        }
        fd_ = -1;
        return;
    }
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
    clients_.resize(cfg_.max_clients);
    buf_.resize(cfg_.max_datagram + 1);
}

Server::~Server()
{
    if (fd_ >= 0)
    {
        close(fd_);
    }
}

Server::Client* Server::client_for(const void* address, int64_t now)
{
    const auto& from = *static_cast<const sockaddr_in*>(address);
    Client* lru = nullptr;
    for (auto& client : clients_)
    {
        if (client.live && same_addr(client.addr, from))
        {
            return &client;
        }
        if (!lru || !client.live || (lru->live && client.last_ms < lru->last_ms))
        {
            lru = &client;
        }
    }
    if (lru->live)
    {
        ++stats_.evicted;
    }
    lru->live = true;
    lru->addr.assign(reinterpret_cast<const uint8_t*>(&from),
                     reinterpret_cast<const uint8_t*>(&from) + sizeof(from));
    lru->id = next_id_++;
    lru->last_ms = now;
    return lru;
}

void Server::on_readable()
{
    for (;;)
    {
        sockaddr_in from {};
        socklen_t len = sizeof(from);
        const ssize_t n = recvfrom(fd_, buf_.data() + 1, cfg_.max_datagram, MSG_DONTWAIT,
                                   reinterpret_cast<sockaddr*>(&from), &len);
        if (n <= 0)
        {
            return;
        }
        if (static_cast<size_t>(n) > cfg_.max_datagram - 1)
        {
            ++stats_.too_big;
            continue;
        }
        const int64_t now = hooks_.now_ms();
        Client* client = client_for(&from, now);
        client->last_ms = now;
        buf_[0] = client->id;
        if (hooks_.send_air(buf_.data(), static_cast<size_t>(n) + 1))
        {
            ++stats_.requests;
        }
        else
        {
            ++stats_.send_fail;
        }
    }
}

void Server::on_air(const uint8_t* data, size_t len)
{
    if (len < 2)
    {
        ++stats_.bad;
        return;
    }
    const Client* owner = nullptr;
    for (const auto& client : clients_)
    {
        if (client.live && client.id == data[0])
        {
            owner = &client;
            break;
        }
    }
    if (!owner)
    {
        ++stats_.no_client;
        return;
    }
    sockaddr_in addr {};
    std::memcpy(&addr, owner->addr.data(), sizeof(addr));
    if (sendto(fd_, data + 1, len - 1, 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ++stats_.send_fail;
        return;
    }
    ++stats_.replies;
}

void Server::tick()
{
    const int64_t now = hooks_.now_ms();
    for (auto& client : clients_)
    {
        if (client.live && now - client.last_ms > cfg_.idle_ms)
        {
            client.live = false;
        }
    }
}

}  // namespace mplane_relay
