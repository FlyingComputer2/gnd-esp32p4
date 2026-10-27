#include "core/rs_block_erasure.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>
using namespace std::chrono_literals;
int main() {
    using vstreamer::rs_block_erasure;
    rs_block_erasure tx, rx;
    assert(tx.init(3,5,20,1441));
    auto now=std::chrono::steady_clock::time_point{};
    rx.set_clock([&]{return now;});
    for(int block=0;block<3;++block) {
        std::vector<std::vector<uint8_t>> wire;
        for(int i=0;i<3;++i) {
            uint8_t p=uint8_t(block*3+i);
            std::vector<std::vector<uint8_t>> batch;
            tx.push_app(&p,1,&batch);
            wire.insert(wire.end(),batch.begin(),batch.end());
        }
        assert(wire.size()==5);
        auto base=std::chrono::steady_clock::time_point{}+block*100ms;
        vstreamer::fec_rx_payload_list out;
        if(block<2) {
            for(unsigned i=0;i<5;++i) {
                now=base+i*15ms;
                rx.push_air(wire[i].data(),wire[i].size(),&out);
            }
        } else {
            now=base+15ms;rx.push_air(wire[1].data(),wire[1].size(),&out);
            now=base+30ms;rx.push_air(wire[2].data(),wire[2].size(),&out);
            now=base+86ms;rx.poll_rx(&out);
            assert(rx.given_up_blocks()==0);
            now=base+90ms;rx.push_air(wire[4].data(),wire[4].size(),&out);
            assert(out.size()==3);
            for(int i=0;i<3;++i) assert(out[i].size()==1 && out[i].u8()[0]==block*3+i);
            assert(rx.recovered()==1 && rx.given_up_blocks()==0);
        }
    }
    puts("P4 FEC: delayed parity within the embedded scheduling margin recovers without a gap");
}
