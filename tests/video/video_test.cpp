#include "components/stream_receiver.hpp"
#include "core/stream_header.hpp"
#include "core/rtp_h264.hpp"
#include "core/stream_telemetry.hpp"
#include "video_fanout.hpp"
#include "multicast_egress.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <thread>
#include <cstdio>
#include <mutex>
#include <deque>

int main(int argc, char **) {
    const bool direct = argc > 1;
    std::mutex queue_mutex;
    std::deque<bfc::shared_sized_buffer> input, reports;
    vstreamer::stream_receiver receiver;
    assert(receiver.configure("listen", "127.0.0.1:21982") == 0);
    assert(receiver.configure("max_datagram", "1445") == 0);
    if (direct) {
        assert(receiver.set_datagram_transport({}, {}) == -EINVAL);
        assert(receiver.set_datagram_transport([&]() -> std::optional<bfc::shared_sized_buffer> {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (input.empty()) return {};
            auto p = std::move(input.front()); input.pop_front(); return p;
        }, [&](const uint8_t *data, size_t size) {
            std::lock_guard<std::mutex> lock(queue_mutex);
            reports.push_back(bfc::shared_sized_buffer::copy_from(data, size)); return true;
        }) == 0);
    }
    assert(receiver.open() == 0);
    if (direct) assert(receiver.set_datagram_transport({}, {}) == -EBUSY);
    int fd = socket(AF_INET, SOCK_DGRAM, 0); assert(fd >= 0);
    sockaddr_in destination{}; destination.sin_family=AF_INET;
    destination.sin_port=htons(21982); destination.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    std::vector<uint8_t> au{0,0,0,1,0x65}; au.resize(3005,0x55);
    vstreamer::rtp_h264_packer packer({300,96,0x1234,30});
    assert(packer.pack_annexb(au.data(),au.size(),0) == 0);
    std::vector<std::vector<uint8_t>> rtp;
    uint8_t bytes[2048];
    while(packer.pending()) {
        int n=packer.pop_datagram(bytes,sizeof(bytes)); assert(n>0); rtp.emplace_back(bytes,bytes+n);
    }
    vstreamer::rs_block_erasure fec; assert(fec.init(3,5,20,1441));
    std::vector<std::vector<uint8_t>> shards;
    for(auto &p:rtp) {
        std::vector<std::vector<uint8_t>> batch; fec.push_app(p.data(),p.size(),&batch);
        shards.insert(shards.end(),batch.begin(),batch.end());
    }
    std::vector<std::vector<uint8_t>> tail; fec.flush(&tail);
    shards.insert(shards.end(),tail.begin(),tail.end());
    for(size_t i=0;i<shards.size();++i) {
        if(i%5==0 && i+4<shards.size()) continue; // Recover one missing data shard per block.
        std::vector<uint8_t> wire(4+shards[i].size());
        vstreamer::stream_header_write(wire.data(),{uint16_t(i),true,true,0});
        std::copy(shards[i].begin(),shards[i].end(),wire.begin()+4);
        if (direct) {
            std::lock_guard<std::mutex> lock(queue_mutex);
            input.push_back(bfc::shared_sized_buffer::copy_from(wire.data(), wire.size()));
        } else assert(sendto(fd,wire.data(),wire.size(),0,reinterpret_cast<sockaddr*>(&destination),sizeof(destination))==ssize_t(wire.size()));
    }
    rover::VideoFanout fanout;
    size_t received=0;
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(received<rtp.size() && std::chrono::steady_clock::now()<deadline) {
        vstreamer::component_pdu p;
        if(receiver.output(p)==0) { fanout.push(p.sdu); ++received; }
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(received==rtp.size());
    vstreamer::rtp_h264_depacketizer depay;
    unsigned frames=0;
    for(size_t i=0;i<rtp.size();++i) {
        auto usb=fanout.pop(0), multicast=fanout.pop(1); assert(usb && multicast);
        assert(usb->size()==rtp[i].size() && multicast->size()==rtp[i].size());
        assert(std::equal(rtp[i].begin(),rtp[i].end(),usb->u8()));
        assert(std::equal(rtp[i].begin(),rtp[i].end(),multicast->u8()));
        std::vector<uint8_t> frame;
        if(depay.feed(usb->u8(),usb->size(),&frame)>0) { assert(frame==au); ++frames; }
    }
    assert(frames==1);
    // Stall USB while multicast keeps consuming: only USB's oldest packets evict.
    for(unsigned i=0;i<rover::VideoFanout::capacity+68;++i) {
        uint8_t marker=uint8_t(i); fanout.push(bfc::shared_sized_buffer::copy_from(&marker,1));
        auto packet=fanout.pop(1); assert(packet && packet->u8()[0]==uint8_t(i));
    }
    assert(fanout.drops(0)==68 && fanout.drops(1)==0);
    auto oldest=fanout.pop(0); assert(oldest && oldest->u8()[0]==68);
    // A full recovery burst fits Ethernet while an idle USB branch remains bounded.
    rover::VideoFanout burst;
    for(unsigned i=0;i<64;++i) {
        uint8_t marker=uint8_t(i);
        burst.push(bfc::shared_sized_buffer::copy_from(&marker,1));
    }
    assert(burst.drops(0)==0 && burst.drops(1)==0);
    for(unsigned i=0;i<64;++i) {
        auto packet=burst.pop(1); assert(packet && packet->u8()[0]==uint8_t(i));
    }
    for(unsigned i=0;i<200;++i) {
        uint8_t marker=uint8_t(i);
        burst.push(bfc::shared_sized_buffer::copy_from(&marker,1));
    }
    assert(burst.drops(1)==72);
    auto ethernet_oldest=burst.pop(1); assert(ethernet_oldest && ethernet_oldest->u8()[0]==72);
    // Transient Ethernet descriptor exhaustion retains the head and ordering.
    rover::VideoFanout pressure;
    rover::MulticastEgress egress;
    for (uint8_t marker : {1,2}) pressure.push(bfc::shared_sized_buffer::copy_from(&marker,1));
    unsigned attempts = 0;
    std::vector<uint8_t> delivered;
    auto send = [&](const bfc::shared_sized_buffer &packet) {
        if (++attempts <= 2) return ENOMEM;
        delivered.push_back(packet.u8()[0]); return 0;
    };
    assert(egress.step(pressure,send)==rover::MulticastEgress::Result::retry);
    assert(egress.step(pressure,send)==rover::MulticastEgress::Result::retry);
    assert(egress.step(pressure,send)==rover::MulticastEgress::Result::sent);
    assert(egress.step(pressure,send)==rover::MulticastEgress::Result::sent);
    assert((delivered==std::vector<uint8_t>{1,2}));
    assert(egress.step(pressure,send)==rover::MulticastEgress::Result::empty);
    uint8_t marker=3; pressure.push(bfc::shared_sized_buffer::copy_from(&marker,1));
    for(unsigned i=1;i<rover::MulticastEgress::max_retries;++i)
        assert(egress.step(pressure,[](const auto &){return ENOBUFS;})==rover::MulticastEgress::Result::retry);
    assert(egress.step(pressure,[](const auto &){return ENOBUFS;})==rover::MulticastEgress::Result::failed);
    // Stock receiver owns reverse feedback, independently of either consumer.
    bool report_seen=false;
    while(std::chrono::steady_clock::now()<deadline) {
        int n = -1;
        if (direct) {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (!reports.empty()) {
                auto report = std::move(reports.front()); reports.pop_front();
                n = int(report.size()); std::copy(report.u8(), report.u8()+n, bytes);
            }
        } else n=recv(fd,bytes,sizeof(bytes),MSG_DONTWAIT);
        if(n>0) {
            vstreamer::stream_header h{}; const uint8_t *payload; size_t len;
            assert(vstreamer::stream_header_parse(bytes,n,&h,&payload,&len)==0 && !h.is_stream_data);
            vstreamer::stream_link_report report{};
            assert(vstreamer::stream_link_report_decode(payload,len,&report)==0);
            if(report.counters.fec_packet_received==rtp.size()) { report_seen=true; break; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(report_seen);
    receiver.close(); close(fd);
    puts("P4 RX: FEC recovery, identical USB/multicast RTP, isolated queues, reverse feedback passed");
}
