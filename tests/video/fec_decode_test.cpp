#include "core/rs_block_erasure.hpp"
#include <algorithm>
#include <cassert>
#include <numeric>
#include <random>
#include <cstdio>
// Shard bodies: a data shard after its 3-byte header, a parity shard after its 4-byte one.
static std::vector<uint8_t> body(const bfc::shared_sized_buffer &shard, size_t header) {
    return std::vector<uint8_t>(shard.u8()+header, shard.u8()+shard.size());
}
int main() {
    using vstreamer::rs_block_erasure;
    std::mt19937 random(12345);
    unsigned cases=0;
    for(int count : {21,17,1}) {
        rs_block_erasure tx;
        assert(tx.init(21,31,1445));
        std::vector<std::vector<uint8_t>> original,wire;
        rs_block_erasure::tx_out out;
        for(int i=0;i<count;++i) {
            std::vector<uint8_t> payload(100+random()%1277);
            for(auto &byte:payload) byte=uint8_t(random());
            original.push_back(payload);
            tx.push_app(payload.data(),payload.size(),&out);
            const size_t data=out.shards.size()-size_t(out.parity);
            for(size_t s=0;s<out.shards.size();++s)
                wire.push_back(body(out.shards[s],s<data?rs_block_erasure::k_data_header_len
                                                        :rs_block_erasure::k_parity_header_len));
        }
        if(count<21) {
            tx.flush(&out);
            for(auto &s:out.shards) wire.push_back(body(s,rs_block_erasure::k_parity_header_len));
        }
        assert(wire.size()==unsigned(count+10));
        for(int lost=0;lost<=10;++lost) for(int trial=0;trial<24;++trial) {
            std::vector<int> positions(wire.size());std::iota(positions.begin(),positions.end(),0);
            std::shuffle(positions.begin(),positions.end(),random);
            std::vector<bfc::shared_sized_buffer> data(static_cast<size_t>(count)), parity(10);
            unsigned missing_data=0;
            for(int i=0;i<lost;++i) if(positions[i]<count) ++missing_data;
            for(unsigned i=lost;i<positions.size();++i) {
                const int p=positions[i];
                auto storage=bfc::shared_sized_buffer::allocate(wire[p].size()+15);
                storage.resize(wire[p].size()+15);
                const size_t offset=unsigned(trial)%16;
                std::copy(wire[p].begin(),wire[p].end(),storage.u8()+offset);
                (p<count?data[size_t(p)]:parity[size_t(p-count)])=storage.subview(offset,wire[p].size());
            }
            const auto received=data;
            int recovered=-1;
            assert(rs_block_erasure::decode_rows(count,10,&data,parity,&recovered));
            assert(recovered==int(missing_data));
            for(unsigned i=0;i<data.size();++i) {
                assert(data[i].size()==original[i].size());
                assert(std::equal(original[i].begin(),original[i].end(),data[i].u8()));
                // Received payloads stay views of their storage; recovered ones follow the
                // aligned length prefix of their decode row.
                if(!received[i].empty()) assert(data[i].u8()==received[i].u8());
                else assert((reinterpret_cast<uintptr_t>(data[i].u8()-2)&15)==0);
            }
            parity.clear(); // Recovered views must survive every input owner going away.
            for(unsigned i=0;i<data.size();++i)
                assert(std::equal(original[i].begin(),original[i].end(),data[i].u8()));
            ++cases;
        }
    }
    printf("FEC data-only decode: %u full/partial 21/31 blocks with 0..10 mixed erasures passed\n",cases);
}
