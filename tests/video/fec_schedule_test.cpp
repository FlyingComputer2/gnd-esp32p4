#include "core/rs_block_erasure.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>
using namespace std::chrono_literals;
int main() {
    using vstreamer::rs_block_erasure;
    rs_block_erasure tx, rx;
    assert(tx.init(3,5,1445));
    auto now=std::chrono::steady_clock::time_point{};
    rx.set_clock([&]{return now;});
    // Spread 75 ms over 5 shards: data 15 ms apart, parity due at +45 and +60 ms.
    for(int block=0;block<3;++block) {
        std::vector<std::vector<uint8_t>> wire;
        for(int i=0;i<3;++i) {
            uint8_t p=uint8_t(block*3+i);
            rs_block_erasure::tx_out out;
            tx.push_app(&p,1,&out);
            for(auto &s:out.shards) wire.emplace_back(s.u8(),s.u8()+s.size());
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
            // Data 1 is lost; the last parity, due at +60 ms, comes 30 ms late. The 40 ms
            // embedded margin floor still waits for it (a 5 ms floor gives up at +65 ms).
            now=base;rx.push_air(wire[0].data(),wire[0].size(),&out);
            assert(out.size()==1 && out[0].u8()[0]==block*3);
            now=base+30ms;rx.push_air(wire[2].data(),wire[2].size(),&out);
            now=base+86ms;rx.poll_rx(&out);
            assert(out.empty() && rx.given_up_blocks()==0);
            now=base+90ms;rx.push_air(wire[4].data(),wire[4].size(),&out);
            assert(out.size()==2);
            for(int i=1;i<3;++i) assert(out[i-1].size()==1 && out[i-1].u8()[0]==block*3+i);
            assert(rx.recovered()==1 && rx.given_up_blocks()==0);
        }
    }
    puts("P4 FEC: delayed parity within the embedded scheduling margin recovers without a gap");
}
