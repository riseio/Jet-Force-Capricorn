#include "texture_bank.hpp"
#include "contrib/miniz/miniz.h"
#define XXH_INLINE_ALL
#include "contrib/xxhash/xxhash.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <stdexcept>
#include <string>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include "common/rt64_load_types.h"
#include "shared/rt64_f3d_defines.h"
#include "common/rt64_tmem_hasher.h"

namespace jfc::textures {
namespace {
using Bytes = std::vector<uint8_t>;
uint32_t u32(std::span<const uint8_t> b, size_t p, bool little=false) {
    if(p>b.size() || b.size()-p<4) throw std::runtime_error("Truncated texture bank word");
    uint32_t v=0;
    for(unsigned i=0;i<4;++i) v|=uint32_t(b[p+i])<<((little?i:3-i)*8);
    return v;
}
uint32_t u16(std::span<const uint8_t> b,size_t p) {
    if(p>b.size() || b.size()-p<2) throw std::runtime_error("Truncated texture bank halfword");
    return uint32_t(b[p])*256+b[p+1];
}
std::span<const uint8_t> extent(std::span<const uint8_t> b,size_t p,size_t n) {
    if(p>b.size() || n>b.size()-p) throw std::runtime_error("Texture bank extent outside input");
    return b.subspan(p,n);
}
constexpr std::array<uint8_t,7> sizes{3,2,1,0,2,1,0};
constexpr std::array<uint8_t,7> formats{0,0,4,4,3,3,3};

Image decode_tmem(const std::array<uint8_t,4096>& tmem,
    const RT64::LoadTile& tile,uint32_t width,uint32_t height) {
    Image image{width,height,{}};
    image.rgba.resize(size_t(width)*height*4);
    const bool split=tile.siz==3;
    const unsigned mask=split?2047:4095;
    for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
        const unsigned p=((tile.tmem*8+y*tile.line*8+((x<<(split?2:tile.siz))>>1)) ^ ((y&1)?4:0))&mask;
        auto* out=image.rgba.data()+(size_t(y)*width+x)*4;
        if(split) {
            out[0]=tmem[p];out[1]=tmem[(p+1)&mask];out[2]=tmem[p|2048];out[3]=tmem[((p+1)&mask)|2048];
        } else if(tile.siz==2 && tile.fmt==0) {
            const unsigned v=(unsigned(tmem[p])<<8)|tmem[(p+1)&mask];
            out[0]=uint8_t((v>>11)*255/31);out[1]=uint8_t(((v>>6)&31)*255/31);
            out[2]=uint8_t(((v>>1)&31)*255/31);out[3]=(v&1)?255:0;
        } else if(tile.siz==2) {
            out[0]=out[1]=out[2]=tmem[p];out[3]=tmem[(p+1)&mask];
        } else if(tile.siz==1 && tile.fmt==3) {
            out[0]=out[1]=out[2]=(tmem[p]>>4)*17;out[3]=(tmem[p]&15)*17;
        } else if(tile.siz==1) {
            out[0]=out[1]=out[2]=out[3]=tmem[p];
        } else {
            const unsigned v=(tmem[p]>>((x&1)?0:4))&15;
            if(tile.fmt==3) {
                const unsigned i=v&14;
                out[0]=out[1]=out[2]=uint8_t((i<<4)|(i<<1)|(i>>2));out[3]=(v&1)?255:0;
            } else out[0]=out[1]=out[2]=out[3]=uint8_t(v*17);
        }
    }
    return image;
}
}

void visit_bank_tiles(std::span<const uint8_t> rom,const std::function<void(BankTile&&)>& consume) {
    if(rom.size()!=33554432 || XXH3_64bits(rom.data(),rom.size())!=0x44830C263C2DE108ULL)
        throw std::runtime_error("Texture bank requires validated JFG US revision0 ROM");
    constexpr size_t fs=0xB1880,table=0xB1754;
    for(unsigned bank=0;bank<2;++bank) {
        const size_t start=fs+u32(rom,table+bank*8), offsets=fs+u32(rom,table+bank*8+4);
        const size_t end=fs+u32(rom,table+bank*8+8);
        if(start>offsets || offsets>end || end>rom.size() || (end-offsets)%4)
            throw std::runtime_error("Invalid original texture bank boundaries");
        for(unsigned index=0;size_t(index+1)*4<end-offsets;++index) {
            const size_t first=u32(rom,offsets+index*4),last=u32(rom,offsets+(index+1)*4);
            if(last==UINT32_MAX) break;
            if(first>last || last>offsets-start) throw std::runtime_error("Invalid original texture index extent");
            if(first==last) continue;
            auto data=extent(rom,start+first,last-first);
            if(data.size()<32) throw std::runtime_error("Truncated original texture header");
            Bytes storage;
            if(data[25]) {
                const auto size=u32(data,32,true);
                if(size<32 || size>1024*1024) throw std::runtime_error("Invalid texture inflate size");
                storage.resize(size);
                const auto compressed=extent(data,37,data.size()-std::min(data.size(),size_t(37)));
                if(tinfl_decompress_mem_to_mem(storage.data(),size,compressed.data(),compressed.size(),0)!=size)
                    throw std::runtime_error("Invalid texture deflate stream");
                data=storage;
            }
            const unsigned format=data[2]&15,width=data[0],height=data[1];
            const unsigned frames=u16(data,18)>>8,levels=std::max(1U,unsigned(data[27]));
            if(format>=sizes.size() || !width || !height || !frames || levels>8)
                throw std::runtime_error("Unsupported original texture layout at "+std::to_string(bank)+":"+std::to_string(index));
            const bool split=format==0;
            const unsigned frame_bytes=u16(data,22)?u16(data,22):unsigned(data.size()-32)/frames;
            if(size_t(frame_bytes)*frames!=data.size()-32)
                throw std::runtime_error("Texture animation extent mismatch");
            for(unsigned frame=0;frame<frames;++frame) {
                const auto pixels=extent(data,32+size_t(frame)*frame_bytes,frame_bytes);
                std::array<uint8_t,4096> tmem{};
                std::array<uint8_t,4096> alternate;
                alternate.fill(0xA7);
                unsigned texels=0;
                for(unsigned level=0;level<levels;++level) texels+=(width>>level)*(height>>level);

                const unsigned units=split?texels:
                    sizes[format]==2?texels:sizes[format]==1?(texels+1)/2:(texels+3)/4;
                const unsigned loaded=((std::min(units,2048U)*(split?4:2)+7)/8)*8;
                if(loaded>pixels.size()) throw std::runtime_error("Native texture load exceeds original frame");

                if(split) {
                    for(size_t p=0;p<loaded;p+=4) {
                        const size_t q=(p/2)&2047;
                        tmem[q]=pixels[p];tmem[q+1]=pixels[p+1];
                        tmem[q|2048]=pixels[p+2];tmem[(q+1)|2048]=pixels[p+3];
                        alternate[q]=pixels[p];alternate[q+1]=pixels[p+1];
                        alternate[q|2048]=pixels[p+2];alternate[(q+1)|2048]=pixels[p+3];
                    }
                } else {
                    for(size_t p=0;p<loaded;++p) tmem[p&4095]=alternate[p&4095]=pixels[p];
                }
                unsigned address=0;
                for(unsigned level=0;level<levels;++level) {
                    const unsigned w=width>>level,h=height>>level;
                    if(!w || !h) throw std::runtime_error("Invalid authored mip extent");
                    RT64::LoadTile tile{};
                    tile.fmt=formats[format];tile.siz=sizes[format];
                    tile.line=uint16_t(((((width<<(split?2:tile.siz))>>1)>>level)+7)/8);
                    tile.tmem=uint16_t(address/8);
                    BankTile result{bank,index,frame,level};
                    std::copy_n(data.begin(),32,result.original_header.begin());
                    result.original_frame=pixels;
                    result.hash=RT64::TMEMHasher::hash(tmem.data(),tile,uint16_t(w),uint16_t(h),0,6);
                    result.self_contained=result.hash==RT64::TMEMHasher::hash(alternate.data(),tile,uint16_t(w),uint16_t(h),0,6);
                    if(level==0 && !result.self_contained)
                        throw std::runtime_error("Original base texture is not self-contained");
                    result.image=decode_tmem(tmem,tile,w,h);
                    consume(std::move(result));
                    address+=tile.line*8*h;
                }
            }
        }
    }
}
}
