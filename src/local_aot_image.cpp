#include "local_aot_image.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace jfc::local_aot {
std::vector<uint8_t> reconstruct_image(std::span<const uint8_t> recipe,
                                     std::span<const uint8_t> rom) {
    const auto invalid = [] { throw std::runtime_error("Invalid offline native-code image recipe"); };
    constexpr std::array<uint8_t,8> magic{'J','F','C','E','L','F','1',0};
    if (recipe.size() < 20 || !std::equal(magic.begin(),magic.end(),recipe.begin())) invalid();
    auto word = [&](size_t offset) -> uint32_t {
        if (offset > recipe.size() || recipe.size()-offset < 4) invalid();
        return uint32_t(recipe[offset]) | uint32_t(recipe[offset+1])<<8 |
            uint32_t(recipe[offset+2])<<16 | uint32_t(recipe[offset+3])<<24;
    };
    const uint32_t size=word(8), count=word(12), patches=word(16);
    if (!size || size>16*1024*1024 || count>512 || patches>65536 ||
        recipe.size()!=20ull+size+12ull*(count+patches)) invalid();
    std::vector<uint8_t> output(recipe.begin()+20,recipe.begin()+20+size);
    struct Segment {uint32_t first,end;};
    std::array<Segment,512> segments{};
    size_t cursor=20+size;
    for (uint32_t i=0;i<count;++i,cursor+=12) {
        const uint32_t offset=word(cursor), source=word(cursor+4), length=word(cursor+8);
        if (!length || length>size || offset>size-length || source>rom.size() || length>rom.size()-source) invalid();
        for (uint32_t j=0;j<i;++j)
            if (offset<segments[j].end && offset+length>segments[j].first) invalid();
        if (std::any_of(output.begin()+offset,output.begin()+offset+length,[](uint8_t byte){return byte!=0;}))
            throw std::runtime_error("Offline kit unexpectedly contains loadable game payload");
        segments[i]={offset,offset+length};
        std::copy_n(rom.begin()+source,length,output.begin()+offset);
    }
    std::sort(segments.begin(),segments.begin()+count,[](auto a,auto b){return a.first<b.first;});
    uint32_t previous=0, segment=0;
    for (uint32_t i=0;i<patches;++i,cursor+=12) {
        const uint32_t offset=word(cursor), kind=word(cursor+4), target=word(cursor+8);
        if ((i && offset<=previous) || offset%4 || (target|0x80000000U)>=0x90000000U) invalid();
        previous=offset;
        while (segment<count && segments[segment].end<=offset) ++segment;
        if (segment==count || offset<segments[segment].first || segments[segment].end-offset<4) invalid();
        uint32_t value=uint32_t(output[offset])<<24 | uint32_t(output[offset+1])<<16 |
            uint32_t(output[offset+2])<<8 | output[offset+3];
        switch (kind) {
        case 2: value=target; break;
        case 4: value=(value&0xFC000000U)|((target>>2)&0x03FFFFFFU); break;
        case 5: value=(value&0xFFFF0000U)|(((target>>16)+((target>>15)&1))&0xFFFF); break;
        case 6: value=(value&0xFFFF0000U)|(target&0xFFFF); break;
        default: invalid();
        }
        for (unsigned lane=0;lane<4;++lane) output[offset+lane]=uint8_t(value>>(24-lane*8));
    }
    return output;
}
}
