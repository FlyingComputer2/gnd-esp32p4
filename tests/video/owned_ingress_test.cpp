#include "components/stream_receiver.hpp"
#include "core/rs_block_erasure.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

static void check(bool fec_enabled) {
    std::vector<uint8_t> app(1003);
    for (size_t i=0; i<app.size(); ++i) app[i]=uint8_t(i);
    // fec none is a data-only stream (k = n = 1); with FEC the data shard leaves before parity.
    vstreamer::rs_block_erasure tx;
    assert(tx.init(1,fec_enabled?2:1,1445));
    vstreamer::rs_block_erasure::tx_out air;
    tx.push_app(app.data(),app.size(),&air);
    assert(air.shards.size()==(fec_enabled?2u:1u));
    const auto &shard=air.shards.front();
    const size_t size=shard.size();
    auto *bytes=new std::byte[size];
    std::atomic<unsigned> released{0};
    std::memcpy(bytes,shard.u8(),size);
    const auto *expected=reinterpret_cast<uint8_t*>(bytes)+vstreamer::rs_block_erasure::k_data_header_len;
    std::deque<bfc::shared_sized_buffer> queue;
    std::mutex mu;
    queue.push_back(bfc::shared_sized_buffer::adopt(bytes,size,size,[&](std::byte *p){
        ++released; delete[] p;
    }));
    vstreamer::stream_receiver rx;
    assert(rx.set_datagram_transport([&]() -> std::optional<bfc::shared_sized_buffer> {
        std::lock_guard<std::mutex> lock(mu);
        if(queue.empty()) return {};
        auto p=std::move(queue.front());queue.pop_front();return p;
    },[](const uint8_t*,size_t){return true;})==0);
    assert(rx.open()==0);
    vstreamer::component_pdu output;
    bool ready=false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(std::chrono::steady_clock::now()<deadline) {
        if(rx.output(output)==0) {ready=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(ready && output.sdu.u8()==expected);
    rx.close();
    assert(released==0); // The owner survives queue drain and receiver shutdown.
    assert(output.sdu.size()==app.size());
    assert(std::memcmp(output.sdu.u8(),app.data(),app.size())==0);
    auto second=output.sdu;
    output={};
    assert(released==0);
    second={};
    assert(released==1); // Last consumer alone releases the original storage.
}
int main() { check(false);check(true); }
