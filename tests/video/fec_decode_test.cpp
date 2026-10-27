#include "core/rs_block_erasure.hpp"
#include <algorithm>
#include <cassert>
#include <numeric>
#include <random>
#include <cstdio>
int main() {
    std::mt19937 random(12345);
    unsigned cases=0;
    for(int count : {21,17,1}) {
        vstreamer::rs_block_erasure tx,rx;
        assert(tx.init(21,31,20,1441));
        std::vector<std::vector<uint8_t>> original,wire;
        for(int i=0;i<count;++i) {
            std::vector<uint8_t> payload(100+random()%1277);
            for(auto &byte:payload) byte=uint8_t(random());
            original.push_back(payload);
            std::vector<std::vector<uint8_t>> batch;
            tx.push_app(payload.data(),payload.size(),&batch);
            wire.insert(wire.end(),batch.begin(),batch.end());
        }
        std::vector<std::vector<uint8_t>> tail;tx.flush(&tail);
        wire.insert(wire.end(),tail.begin(),tail.end());
        assert(wire.size()==unsigned(count+10));
        for(int lost=0;lost<=10;++lost) for(int trial=0;trial<24;++trial) {
            std::vector<int> positions(wire.size());std::iota(positions.begin(),positions.end(),0);
            std::shuffle(positions.begin(),positions.end(),random);
            std::unordered_map<int,bfc::shared_sized_buffer> frags;
            unsigned missing_data=0;
            for(int i=0;i<lost;++i) if(positions[i]<count) ++missing_data;
            for(unsigned i=lost;i<positions.size();++i) {
                int p=positions[i],index=p<count?p:21+(p-count);
                auto storage=bfc::shared_sized_buffer::allocate(wire[p].size()+15);
                storage.resize(wire[p].size()+15);
                const size_t offset=unsigned(trial)%16;
                std::copy(wire[p].begin(),wire[p].end(),storage.u8()+offset);
                frags[index]=storage.subview(offset,wire[p].size());
            }
            vstreamer::fec_rx_payload_list out;
            int recovered=-1;
            assert(rx.decode_block(21,31,count,frags,&out,&recovered));
            assert(out.size()==original.size() && recovered==int(missing_data));
            for(unsigned i=0;i<out.size();++i) {
                assert(out[i].size()==original[i].size());
                assert(std::equal(original[i].begin(),original[i].end(),out[i].u8()));
                // Existing systematic payloads remain views of received storage.
                auto got=frags.find(int(i));
                if(got!=frags.end()) assert(out[i].u8()==got->second.u8()+7);
                else assert((reinterpret_cast<uintptr_t>(out[i].u8()-2)&15)==0);
            }
            frags.clear(); // Held output views must survive all input owners disappearing.
            for(unsigned i=0;i<out.size();++i)
                assert(std::equal(original[i].begin(),original[i].end(),out[i].u8()));
            ++cases;
        }
    }
    printf("FEC data-only decode: %u full/partial 21/31 blocks with 0..10 mixed erasures passed\n",cases);
}
