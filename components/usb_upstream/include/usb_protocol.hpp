#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace rover::usb
{
constexpr size_t header_size = 20, max_payload = 2048;
enum Kind : uint8_t
{
    request = 1,
    reply = 2,
    event = 3,
    data = 4,
    credit = 5
};
inline uint16_t u16(const uint8_t *p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline uint32_t u32(const uint8_t *p) { return uint32_t(u16(p)) | (uint32_t(u16(p + 2)) << 16); }
inline void put16(uint8_t *p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}
inline void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}
struct Record
{
    Kind kind;
    uint16_t channel;
    uint32_t session, sequence;
    std::vector<uint8_t> payload;
};
inline std::vector<uint8_t> encode(const Record &r)
{
    if (r.payload.size() > max_payload)
        return {};
    std::vector<uint8_t> out(header_size + r.payload.size());
    memcpy(out.data(), "RGP4", 4);
    out[4] = 2;
    out[5] = r.kind;
    put16(out.data() + 8, r.channel);
    put16(out.data() + 10, r.payload.size());
    put32(out.data() + 12, r.session);
    put32(out.data() + 16, r.sequence);
    if (!r.payload.empty())
        memcpy(out.data() + 20, r.payload.data(), r.payload.size());
    return out;
}
class Parser
{
    std::array<uint8_t, header_size + max_payload> buf{};
    size_t used = 0;
    uint32_t session_ = 0;

  public:
    bool failed = false;
    void reset(uint32_t session)
    {
        used = 0;
        session_ = session;
        failed = false;
    }
    void feed(const uint8_t *p, size_t n, const std::function<void(const Record &)> &cb)
    {
        while (n-- && !failed)
        {
            buf[used++] = *p++;
            if (used < header_size)
                continue;
            // Stale bytes may still be in an endpoint after RESET_SESSION.
            if (memcmp(buf.data(), "RGP4", 4) || u32(buf.data() + 12) != session_ || !session_)
            {
                memmove(buf.data(), buf.data() + 1, --used);
                continue;
            }
            const auto kind = buf[5];
            const auto len = u16(buf.data() + 10);
            if (buf[4] != 2 || kind < request || kind > credit || u16(buf.data() + 6) ||
                len > max_payload ||
                ((kind <= event) && (u16(buf.data() + 8) != 0 || len > 1024)) ||
                (kind == data && !len) ||
                (kind == credit && (len != 2 || u32(buf.data() + 16) != 0)))
            {
                failed = true;
                break;
            }
            if (used == header_size + len)
            {
                Record r{Kind(kind), u16(buf.data() + 8), session_, u32(buf.data() + 16), {}};
                r.payload.assign(buf.begin() + 20, buf.begin() + used);
                used = 0;
                cb(r);
            }
        }
    }
};
} // namespace rover::usb
