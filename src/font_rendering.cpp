#include "jfc_recomp.h"
#include <stdexcept>

extern "C" void jfc_bound_font_tiles(uint8_t* rdram, uint32_t definition) {
    const auto valid=[](uint32_t p,uint32_t size) {
        const auto segment=p&0xE0000000U;
        return (segment==0x80000000U || segment==0xA0000000U) && !(p&3U) &&
            (p&0x1FFFFFFFU)<=8U*1024U*1024U-size;
    };
    if(!rdram || !valid(definition,16))
        throw std::runtime_error("Invalid native font definition address");
    const uint32_t list=MEM_W(12,definition);

    if(!list) return;
    if(!valid(list,128) || !MEM_BU(0,definition) || !MEM_BU(1,definition))
        throw std::runtime_error("Invalid native font display list");
    for(uint32_t offset=0;offset<128;offset+=8) {
        const uint32_t first=MEM_W(offset,list),second=MEM_W(offset+4,list);
        if((first>>24)==0xB8 || (first>>24)==0xDF) break;
        if((first>>24)==0xF5 && ((second>>24)&7U)!=7U) {
            constexpr uint32_t masks=(15U<<14)|(15U<<4);
            constexpr uint32_t clamp=(2U<<18)|(2U<<8);
            MEM_W(offset+4,list)=(second&~masks)|clamp;
        }
    }
}
