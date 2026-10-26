#include "components/stream_receiver.hpp"
#include "core/stream_header.hpp"
#include "core/rtp_h264.hpp"
#include "core/stream_telemetry.hpp"
#include "video_fanout.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <thread>
#include <cstdio>

int main() {
    vstreamer::stream_receiver receiver;
    assert(receiver.configure("listen", "127.0.0.1:21982") == 0);
    assert(receiver.configure("max_datagram", "1445") == 0);
    assert(receiver.open() == 0);
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
        assert(sendto(fd,wire.data(),wire.size(),0,reinterpret_cast<sockaddr*>(&destination),sizeof(destination))==ssize_t(wire.size()));
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
    for(unsigned i=0;i<100;++i) {
        uint8_t marker=uint8_t(i); fanout.push(bfc::shared_sized_buffer::copy_from(&marker,1));
        auto packet=fanout.pop(1); assert(packet && packet->u8()[0]==i);
    }
    assert(fanout.drops(0)==68 && fanout.drops(1)==0);
    auto oldest=fanout.pop(0); assert(oldest && oldest->u8()[0]==68);
    // Stock receiver owns reverse feedback, independently of either consumer.
    bool report_seen=false;
    while(std::chrono::steady_clock::now()<deadline) {
        int n=recv(fd,bytes,sizeof(bytes),MSG_DONTWAIT);
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
