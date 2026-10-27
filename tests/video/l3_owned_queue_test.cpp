#include "endpoint/QueueUpstream.h"
#include "frames/LCHeader.h"
#include <atomic>
#include <cassert>
#include <cstring>
using namespace winject;
int main() {
    auto cv=std::make_shared<Upstream::Reactor>();
    auto app_tx=std::make_shared<ReactiveLossyQueue>();app_tx->set_capacity(64);
    auto app_rx=std::make_shared<ReactiveLossyQueue>();app_rx->set_capacity(64);
    auto radio_tx=std::make_shared<BoundedLossyQueue>();
    UpstreamConfig cfg;cfg.mode=UpstreamMode::queue;cfg.bus_tx=cfg.bus_rx=7;
    auto up=std::make_shared<QueueUpstream>(cv,cfg,radio_tx,app_tx,app_rx);
    up->bind_callbacks();
    constexpr size_t payload_size=1003;
    const size_t size=LCHeader::k_len+payload_size;
    auto *bytes=new std::byte[size];
    auto *wire=reinterpret_cast<uint8_t*>(bytes);
    LCHeader::write(wire,7,1,false);
    for(size_t i=0;i<payload_size;++i)wire[LCHeader::k_len+i]=uint8_t(i);
    std::atomic<unsigned> released{0};
    up->rx_queue()->push(bfc::shared_sized_buffer::adopt(bytes,size,size,[&](std::byte *p){++released;delete[] p;}));
    cv->get_timer().wait_ms(5,[cv]{cv->stop();});cv->run();
    auto received=up->app_transport()->receive();
    assert(received && received->size()==payload_size);
    assert(received->u8()==wire+LCHeader::k_len);
    assert(up->rx_counters().accepted_packets==1 && up->app_rx_packets()==1);
    assert(up->rx_counters().accepted_bytes==payload_size);
    up->teardown();up.reset();
    assert(released==0);
    for(size_t i=0;i<payload_size;++i)assert(received->u8()[i]==uint8_t(i));
    received.reset();assert(released==1);
}
