#define ROVER_USB_HOST_TEST
#include "../components/usb_upstream/usb_upstream.cpp"
#include <cassert>
#include <iostream>

static Record decode(const std::vector<uint8_t> &bytes, uint32_t session)
{
    Record out{};
    Parser p;
    p.reset(session);
    unsigned n = 0;
    p.feed(bytes.data(), bytes.size(),
           [&](const Record &r)
           {
               out = r;
               ++n;
           });
    assert(n == 1 && !p.failed);
    return out;
}
static Record command(uint32_t seq, const char *text)
{
    Record r{request, 0, session, seq, {text, text + strlen(text)}};
    on_record(r);
    auto reply = next_record();
    assert(!reply.empty());
    return decode(reply, session);
}
int main()
{
    // Identical golden bytes are asserted by the Android JVM codec test.
    auto golden = encode({data, 65535, 0x01020304, 0x11223344, {1, 2, 3}});
    const uint8_t expected[] = {0x52, 0x47, 0x50, 0x34, 2,    4,    0,    0,    255, 255, 3, 0,
                                4,    3,    2,    1,    0x44, 0x33, 0x22, 0x11, 1,   2,   3};
    assert(golden == std::vector<uint8_t>(std::begin(expected), std::end(expected)));
    for (uint16_t id : {0, 1, 2, 255, 256, 32767, 32768, 65535})
    {
        auto bytes = encode({data, id, 7, 1, {1, 2, 3}});
        for (size_t split = 0; split <= bytes.size(); ++split)
        {
            Parser p;
            p.reset(7);
            unsigned n = 0;
            auto cb = [&](const Record &r)
            {
                assert(r.channel == id && r.kind == data);
                ++n;
            };
            p.feed(bytes.data(), split, cb);
            p.feed(bytes.data() + split, bytes.size() - split, cb);
            assert(n == 1);
        }
    }
    using Queue = winject::ReactiveLossyQueue;
    auto in = std::make_shared<Queue>(), out = std::make_shared<Queue>();
    in->set_capacity(8);
    out->set_capacity(8);
    auto backend = std::make_shared<winject::UpstreamQueue>(in, out);
    assert(usb_upstream_register(0, "video", backend) == ESP_OK);
    assert(usb_upstream_register(0, "duplicate", backend) != ESP_OK);
    assert(usb_upstream_register(1, "same transport", backend) != ESP_OK);
    auto in2 = std::make_shared<Queue>(), out2 = std::make_shared<Queue>();
    in2->set_capacity(8);
    out2->set_capacity(8);
    auto telemetry = std::make_shared<winject::UpstreamQueue>(in2, out2);
    assert(usb_upstream_register(65535, "test-telemetry", telemetry) == ESP_OK);
    reset(123);
    auto greeting = command(1, "{\"op\":\"hello\",\"version\":2}");
    assert(greeting.kind == reply);
    auto invalid = command(99, "{\"op\":\"list\",\"offset\":-1}");
    assert(std::string(invalid.payload.begin(), invalid.payload.end()).find("BAD_REQUEST") !=
           std::string::npos);
    assert(!bounded_json(std::vector<uint8_t>(10, '[')));
    command(2, "{\"op\":\"open\",\"channel\":0,\"rx_window\":1}");
    command(3, "{\"op\":\"open\",\"channel\":65535,\"rx_window\":1}");
    on_record({data, 0, 123, 1, {9, 8, 7}});
    auto app = in->pop();
    assert(app && app->size() == 3 && app->u8()[0] == 9);
    auto returned = decode(next_record(), 123);
    assert(returned.kind == credit && returned.channel == 0 && u16(returned.payload.data()) == 1);
    out->push(bfc::shared_sized_buffer::copy_from("v", 1));
    auto video = decode(next_record(), 123);
    assert(video.channel == 0 && video.kind == data);
    // Video exhausts its one receive credit; telemetry still progresses.
    out->push(bfc::shared_sized_buffer::copy_from("x", 1));
    out2->push(bfc::shared_sized_buffer::copy_from("t", 1));
    auto other = decode(next_record(), 123);
    assert(other.channel == 65535 && other.kind == data);
    assert(next_record().empty());
    on_record({credit, 0, 123, 0, {1, 0}});
    auto resumed = decode(next_record(), 123);
    assert(resumed.channel == 0 && resumed.payload[0] == 'x');
    // Reset discards partial wire input and old-session records.
    auto stale = encode({data, 0, 123, 2, {5}});
    reset(456);
    parser.feed(stale.data(), stale.size(), on_record);
    assert(session == 456 && !hello);
    command(4, "{\"op\":\"hello\",\"version\":2}");
    command(5, "{\"op\":\"open\",\"channel\":0,\"rx_window\":1}");
    on_record({credit, 0, 456, 0, {2, 0}});
    assert(session == 0); // Overflow invalidates session.
    reset(789);
    command(6, "{\"op\":\"hello\",\"version\":2}");
    on_record({data, 2, 789, 1, {1}});
    assert(session == 0); // MSP TBA: never misroute to video.
    backend->disconnect();
    assert(!backend->try_send(reinterpret_cast<const uint8_t *>("x"), 1));
    std::cout << "USB v2: u16 codec, direct L3 transport, isolation, credits and reset passed\n";
}
