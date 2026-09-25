#include "texture_reconstruction.hpp"
#include "atomic_file.hpp"
#include "texture_bank.hpp"
#include "contrib/miniz/miniz.h"
#define XXH_INLINE_ALL
#include "contrib/xxhash/xxhash.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <deque>
#include <future>
#include <map>

namespace jfc::textures {
namespace {
using Bytes = std::vector<uint8_t>;
uint32_t word(std::span<const uint8_t> bytes, size_t offset, bool little = false) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Truncated reconstruction metadata");
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= uint32_t(bytes[offset+i]) << ((little ? i : 3-i) * 8);
    return value;
}
void put(Bytes& bytes, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset+i) = uint8_t(value >> (8*i));
}
void validate(const Image& image) {
    if (!image.width || !image.height || image.width > 4096 || image.height > 4096 ||
        image.rgba.size() != size_t(image.width) * image.height * 4)
        throw std::runtime_error("Invalid reconstruction image dimensions/payload");
}
float slope(float a, float b) {
    return a * b <= 0 ? 0 : 2 * a * b / (a + b);
}
float interpolate(float a, float b, float c, float d, float t) {
    const float t2 = t*t, t3 = t2*t;
    const float result = (2*t3-3*t2+1)*b + (t3-2*t2+t)*slope(b-a,c-b) +
        (-2*t3+3*t2)*c + (t3-t2)*slope(c-b,d-c);
    return std::clamp(result, std::min(b,c), std::max(b,c));
}
Image mip(const Image& src) {
    Image out{std::max(1U,src.width/2),std::max(1U,src.height/2),{}};
    out.rgba.resize(size_t(out.width)*out.height*4);

    for (uint32_t y=0;y<out.height;++y) for (uint32_t x=0;x<out.width;++x) {
        const uint32_t x0=x*src.width, x1=(x+1)*src.width;
        const uint32_t y0=y*src.height, y1=(y+1)*src.height;
        std::array<uint64_t,4> sum{};
        for(uint32_t sy=y0/out.height;sy<(y1+out.height-1)/out.height;++sy)
            for(uint32_t sx=x0/out.width;sx<(x1+out.width-1)/out.width;++sx) {
                const uint64_t weight=(std::min(x1,(sx+1)*out.width)-std::max(x0,sx*out.width))*
                    uint64_t(std::min(y1,(sy+1)*out.height)-std::max(y0,sy*out.height));
                for(unsigned c=0;c<4;++c) sum[c]+=src.rgba[(size_t(sy)*src.width+sx)*4+c]*weight;
            }
        const uint64_t total=uint64_t(src.width)*src.height;
        for(unsigned c=0;c<4;++c) out.rgba[(size_t(y)*out.width+x)*4+c]=uint8_t((sum[c]+total/2)/total);
    }
    return out;
}
}

Image decode_tile(std::span<const uint8_t> input) {
    if(input.size()<32) throw std::runtime_error("Truncated original texture header");
    Bytes storage;
    if(input[25]) {
        if(input.size()<37) throw std::runtime_error("Truncated original texture compression header");
        const auto size=word(input,32,true);
        if(size<32 || size>1024*1024) throw std::runtime_error("Invalid original texture decoded length");
        storage.resize(size);
        if(tinfl_decompress_mem_to_mem(storage.data(),storage.size(),input.data()+37,input.size()-37,0)!=size)
            throw std::runtime_error("Invalid or truncated original texture deflate stream");
        input=storage;
    }
    Image result{input[0],input[1],{}};
    const auto format=input[2]&15;
    if(!result.width || !result.height || result.width%4 || (format!=0 && format!=1))
        throw std::runtime_error("Unsupported original reconstruction texture format");
    const unsigned stride=format==0?4:2;
    const auto count=size_t(result.width)*result.height;
    if(input.size()<32+count*stride) throw std::runtime_error("Truncated original texture texels");
    result.rgba.resize(count*4);
    for(uint32_t y=0;y<result.height;++y) for(uint32_t x=0;x<result.width;++x) {

        const size_t from=32+(size_t(y)*result.width+(x^((y&1)?2:0)))*stride;
        const size_t to=(size_t(y)*result.width+x)*4;
        if(format==0) std::copy_n(input.begin()+from,4,result.rgba.begin()+to);
        else {
            const uint32_t v=(uint32_t(input[from])<<8)|input[from+1];
            result.rgba[to]=uint8_t((v>>11)*255/31);
            result.rgba[to+1]=uint8_t(((v>>6)&31)*255/31);
            result.rgba[to+2]=uint8_t(((v>>1)&31)*255/31);
            result.rgba[to+3]=uint8_t((v&1)*255);
        }
    }
    return result;
}

Image clean_grain(const Image& src) {
    validate(src);
    Image out=src;
    const auto luma=[&](uint32_t x,uint32_t y) {
        const auto i=(size_t(y)*src.width+x)*4;
        return (54*int(src.rgba[i])+183*int(src.rgba[i+1])+19*int(src.rgba[i+2])+128)/256;
    };

    for(uint32_t y=2;y+2<src.height;++y) for(uint32_t x=2;x+2<src.width;++x) {
        const int centre=luma(x,y);
        int residual=0,total=0;
        for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
            const int difference=luma(uint32_t(int(x)+dx),uint32_t(int(y)+dy))-centre;
            const int weight=std::max(0,8-std::abs(difference))*(dx==0?2:1)*(dy==0?2:1);
            residual+=weight*difference; total+=weight;
        }
        int delta=(std::abs(residual)+total/2)/total*(residual<0?-1:1);
        const auto distance=std::min({x,y,src.width-1-x,src.height-1-y});
        const int limit=distance==2?1:2;
        int minimum=-limit,maximum=limit;
        const auto i=(size_t(y)*src.width+x)*4;
        for(unsigned c=0;c<3;++c) {
            minimum=std::max(minimum,-int(src.rgba[i+c]));
            maximum=std::min(maximum,255-int(src.rgba[i+c]));
        }
        delta=std::clamp(delta,minimum,maximum);
        for(unsigned c=0;c<3;++c) out.rgba[i+c]=uint8_t(int(src.rgba[i+c])+delta);
    }
    return out;
}

Image restore_detail(const Image& src) {
    validate(src);
    Image out=src;

    const auto luma=[&](uint32_t x,uint32_t y) {
        const auto i=(size_t(y)*src.width+x)*4;
        return (54*int(src.rgba[i])+183*int(src.rgba[i+1])+19*int(src.rgba[i+2])+128)/256;
    };
    for(uint32_t y=2;y+2<src.height;++y) for(uint32_t x=2;x+2<src.width;++x) {
        int sum=0;
        std::array<int,3> low{255,255,255},high{};
        for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
            const auto sx=uint32_t(int(x)+dx),sy=uint32_t(int(y)+dy);
            sum+=luma(sx,sy)*(dx==0?2:1)*(dy==0?2:1);
            for(unsigned c=0;c<3;++c) {
                const int value=src.rgba[(size_t(sy)*src.width+sx)*4+c];
                low[c]=std::min(low[c],value); high[c]=std::max(high[c],value);
            }
        }
        const int residual=16*luma(x,y)-sum;

        const int magnitude=std::max(0,std::abs(residual)-16);
        int delta=(magnitude*4+8)/16*(residual<0?-1:1);
        const auto distance=std::min({x,y,src.width-1-x,src.height-1-y});
        if(distance==2) delta/=2;
        int minimum=-24,maximum=24;
        const auto i=(size_t(y)*src.width+x)*4;

        for(unsigned c=0;c<3;++c) {
            minimum=std::max(minimum,low[c]-int(src.rgba[i+c]));
            maximum=std::min(maximum,high[c]-int(src.rgba[i+c]));
        }
        delta=std::clamp(delta,minimum,maximum);
        for(unsigned c=0;c<3;++c) out.rgba[i+c]=uint8_t(int(src.rgba[i+c])+delta);
    }
    return out;
}

Image reconstruct(const Image& src, uint32_t scale) {
    validate(src);
    if(!scale || scale>16 || src.width>4096/scale || src.height>4096/scale)
        throw std::runtime_error("Invalid reconstruction scale");
    Image out{src.width*scale,src.height*scale,{}};
    out.rgba.resize(size_t(out.width)*out.height*4);

    std::vector<float> horizontal(size_t(src.height)*out.width*4);
    for(uint32_t y=0;y<src.height;++y) for(uint32_t x=0;x<out.width;++x) {
        const float u=(float(x)+0.5f)/scale-0.5f;
        const int ix=int(std::floor(u));
        for(unsigned c=0;c<4;++c) {
            std::array<float,4> p;
            for(int col=-1;col<=2;++col) {
                const int sx=std::clamp(ix+col,0,int(src.width)-1);
                p[col+1]=src.rgba[(size_t(y)*src.width+sx)*4+c];
            }
            horizontal[(size_t(y)*out.width+x)*4+c]=interpolate(p[0],p[1],p[2],p[3],u-ix);
        }
    }
    for(uint32_t y=0;y<out.height;++y) for(uint32_t x=0;x<out.width;++x) {
        const float v=(float(y)+0.5f)/scale-0.5f;
        const int iy=int(std::floor(v));
        for(unsigned c=0;c<4;++c) {
            std::array<float,4> rows;
            for(int row=-1;row<=2;++row) {
                const int sy=std::clamp(iy+row,0,int(src.height)-1);
                rows[row+1]=horizontal[(size_t(sy)*out.width+x)*4+c];
            }
            out.rgba[(size_t(y)*out.width+x)*4+c]=uint8_t(std::lround(
                interpolate(rows[0],rows[1],rows[2],rows[3],v-iy)));
        }
    }
    return out;
}

Image filter_surface(const Image& src, uint32_t texel_scale) {
    validate(src);
    if (!texel_scale || texel_scale > 16 ||
        src.width % texel_scale || src.height % texel_scale)
        throw std::runtime_error("Invalid surface filter texel scale");
    Image out = src;

    const int radius = int(texel_scale);
    const uint32_t border = 2 * texel_scale;
    std::vector<uint8_t> luminance(size_t(src.width) * src.height);
    for (size_t p = 0; p < luminance.size(); ++p) {
        const auto i = p * 4;
        luminance[p] = uint8_t((54 * int(src.rgba[i]) + 183 * int(src.rgba[i+1]) +
            19 * int(src.rgba[i+2]) + 128) / 256);
    }
    const auto luma = [&](uint32_t x, uint32_t y) {
        return int(luminance[size_t(y) * src.width + x]);
    };
    const auto sample_luma = [&](float x, float y) {
        const auto ix = uint32_t(x), iy = uint32_t(y);
        const float u = x - ix, v = y - iy;
        return (1-v) * ((1-u)*luma(ix,iy) + u*luma(ix+1,iy)) +
            v * ((1-u)*luma(ix,iy+1) + u*luma(ix+1,iy+1));
    };
    for (uint32_t y = border; y + border < src.height; ++y)
        for (uint32_t x = border; x + border < src.width; ++x) {
            const int centre = luma(x, y);
            const float gx = float(luma(x+radius,y)-luma(x-radius,y));
            const float gy = float(luma(x,y+radius)-luma(x,y-radius));
            const float length = std::max(1.0f, std::sqrt(gx*gx+gy*gy));
            const float tx = -gy/length, ty = gx/length;
            float residual = 0;
            int total = 0;
            for (int d = -radius; d <= radius; ++d) {
                const float difference = sample_luma(float(x)+tx*d,float(y)+ty*d)-centre;
                const int weight = radius+1-std::abs(d);
                residual += weight*difference;
                total += weight;
            }
            int delta = int(std::lround(residual/total));

            const auto distance = std::min({x, y, src.width-1-x, src.height-1-y});
            const int limit = std::min(8U, distance - border + 1);
            int low = -limit, high = limit;
            const auto i = (size_t(y) * src.width + x) * 4;
            for (unsigned c = 0; c < 3; ++c) {
                low = std::max(low, -int(src.rgba[i+c]));
                high = std::min(high, 255 - int(src.rgba[i+c]));
            }
            delta = std::clamp(delta, low, high);
            for (unsigned c = 0; c < 3; ++c) out.rgba[i+c] = uint8_t(int(src.rgba[i+c]) + delta);
        }
    return out;
}

Bytes make_dds(const Image& image) {
    validate(image);
    uint32_t levels=1;
    for(uint32_t w=image.width,h=image.height;w>1||h>1;++levels) {w=std::max(1U,w/2);h=std::max(1U,h/2);}
    Bytes out(148,0);
    put(out,0,0x20534444);put(out,4,124);put(out,8,0x2100F);
    put(out,12,image.height);put(out,16,image.width);put(out,20,image.width*4);put(out,28,levels);
    put(out,76,32);put(out,80,4);put(out,84,0x30315844);put(out,108,0x401008);
    put(out,128,28);put(out,132,3);put(out,140,1);
    Image level=image;
    for(;;) {
        out.insert(out.end(),level.rgba.begin(),level.rgba.end());
        if(level.width==1 && level.height==1) break;
        level=mip(level);
    }
    return out;
}

Bytes make_pack(std::span<const uint8_t> rom) {
    if(rom.size()!=33554432 || XXH3_64bits(rom.data(),rom.size())!=0x44830C263C2DE108ULL)
        throw std::runtime_error("Reconstruction requires the validated JFG US revision0 ROM");
    constexpr size_t fs=0xB1880, table=0xB1754;
    const size_t base=fs+word(rom,table), offsets=fs+word(rom,table+4);
    const size_t end=fs+word(rom,table+8);
    if(base>offsets || offsets>end || end>rom.size()) throw std::runtime_error("Invalid original texture bank");
    constexpr std::array<unsigned,3> indices{3567,3566,5128};
    constexpr std::array<const char*,3> names{"goldwood_landing_pad.dds","goldwood_landing_highlight.dds","goldwood_ramp.dds"};
    constexpr const char* metadata=R"({"configuration":{"autoPath":"rt64","configurationVersion":3,"hashVersion":5,"defaultOperation":"preload","defaultShift":"half"},"textures":[{"hashes":{"rt64":"5594aa22472a9a44"},"path":"goldwood_landing_pad","preserveOriginalAlpha":true},{"hashes":{"rt64":"d3a75b90291b3ada"},"path":"goldwood_landing_highlight","preserveOriginalAlpha":true},{"hashes":{"rt64":"bbb30f8f4bd2bc3e"},"path":"goldwood_ramp"}]})";
    mz_zip_archive zip{};
    if(!mz_zip_writer_init_heap(&zip,0,1024*1024)) throw std::runtime_error("Cannot allocate reconstruction archive");
    struct ZipOwner { mz_zip_archive* zip; ~ZipOwner(){mz_zip_writer_end(zip);} } owner{&zip};
    const auto add=[&](const char* name,std::span<const uint8_t> bytes) {
        std::tm calendar{}; calendar.tm_year=80; calendar.tm_mday=1; calendar.tm_isdst=-1;
        MZ_TIME_T stamp=std::mktime(&calendar);
        if(stamp == MZ_TIME_T(-1)) throw std::runtime_error("Cannot represent fixed reconstruction ZIP time");
        if(!mz_zip_writer_add_mem_ex_v2(&zip,name,bytes.data(),bytes.size(),nullptr,0,6,0,0,&stamp,nullptr,0,nullptr,0,8))
            throw std::runtime_error("Cannot write reconstruction archive entry");
    };
    add("rt64.json",{reinterpret_cast<const uint8_t*>(metadata),std::char_traits<char>::length(metadata)});
    for(size_t i=0;i<indices.size();++i) {
        if(size_t(indices[i]+2)*4>end-offsets) throw std::runtime_error("Original texture index outside bank");
        const size_t first=word(rom,offsets+indices[i]*4), last=word(rom,offsets+(indices[i]+1)*4);
        if(first>=last || last>offsets-base) throw std::runtime_error("Original texture extent outside bank");
        const auto source=decode_tile(rom.subspan(base+first,last-first));
        if(source.width!=32 || source.height!=(i==2?64U:32U)) throw std::runtime_error("Original texture identity mismatch");
        const auto dds=make_dds(reconstruct_material(source));
        add(names[i],dds);
    }
    void* data=nullptr;size_t size=0;
    if(!mz_zip_writer_finalize_heap_archive(&zip,&data,&size)) throw std::runtime_error("Cannot finalize reconstruction archive");
    std::unique_ptr<void,decltype(&mz_free)> memory(data,mz_free);
    return Bytes(static_cast<uint8_t*>(data),static_cast<uint8_t*>(data)+size);
}

void write_pack(std::span<const uint8_t> rom,FILE* file,const std::function<void(size_t)>& progress,std::stop_token shutdown) {
    if(rom.size()!=33554432 || XXH3_64bits(rom.data(),rom.size())!=0x44830C263C2DE108ULL)
        throw std::runtime_error("Reconstruction requires the validated JFG US revision0 ROM");
    if(!file) throw std::runtime_error("Reconstruction requires an owned seekable output");
    if(shutdown.stop_requested()) throw PreparationCancelled{};
    size_t total=0,unique=0;
    mz_zip_archive zip{};
    if(!mz_zip_writer_init_cfile(&zip,file,MZ_ZIP_FLAG_WRITE_ZIP64))
        throw std::runtime_error("Cannot create reconstruction archive");
    struct Owner {mz_zip_archive* zip;~Owner(){mz_zip_writer_end(zip);}} owner{&zip};
    std::tm calendar{};calendar.tm_year=80;calendar.tm_mday=1;calendar.tm_isdst=-1;
    MZ_TIME_T stamp=std::mktime(&calendar);
    if(stamp==MZ_TIME_T(-1)) throw std::runtime_error("Cannot represent reconstruction archive time");
    const auto add=[&](const std::string& name,std::span<const uint8_t> bytes) {
        if(!mz_zip_writer_add_mem_ex_v2(&zip,name.c_str(),bytes.data(),bytes.size(),nullptr,0,1,0,0,&stamp,nullptr,0,nullptr,0,8))
            throw std::runtime_error("Cannot write reconstruction archive texture");
    };
    struct Pending {std::string name;std::future<Bytes> bytes;};
    std::deque<Pending> pending;
    std::map<uint64_t,uint64_t> identities;
    std::string metadata=R"({"configuration":{"autoPath":"rt64","configurationVersion":3,"hashVersion":6,"defaultOperation":"stream","defaultShift":"half"},"textures":[)";
    const auto finish_one=[&] {
        if(shutdown.stop_requested()) throw PreparationCancelled{};
        const auto bytes=pending.front().bytes.get();
        add(pending.front().name,bytes);pending.pop_front();
    };

    visit_bank_tiles(rom,[&](BankTile&& tile) {
        if(shutdown.stop_requested()) throw PreparationCancelled{};
        ++total;

        if(!tile.self_contained) return;
        const auto content=XXH3_64bits(tile.image.rgba.data(),tile.image.rgba.size());
        const auto [it,inserted]=identities.emplace(tile.hash,content);
        if(!inserted) {
            if(it->second!=content) throw std::runtime_error("Conflicting original texture identity");
            return;
        }
        char hash[17];std::snprintf(hash,sizeof(hash),"%016llx",static_cast<unsigned long long>(tile.hash));
        if(unique++) metadata+=',';
        metadata+=std::string("{\"hashes\":{\"rt64\":\"")+hash+"\"},\"path\":\""+hash+"\",\"preserveOriginalAlpha\":true";

        if(tile.hash==0x5594aa22472a9a44ULL || tile.hash==0xd3a75b90291b3adaULL || tile.hash==0xbbb30f8f4bd2bc3eULL)
            metadata+=",\"operation\":\"preload\"";
        metadata+='}';
        pending.push_back({std::string(hash)+".dds",std::async(std::launch::async,[image=std::move(tile.image)] {
            return make_dds(reconstruct_material(image));
        })});
        if(pending.size()==4) finish_one();
        if(unique%512==0) {

            if(progress) progress(total);
        }
    });
    while(!pending.empty()) finish_one();
    metadata+="]}";
    add("rt64.json",{reinterpret_cast<const uint8_t*>(metadata.data()),metadata.size()});
    if(!mz_zip_writer_finalize_archive(&zip)) throw std::runtime_error("Cannot finalize reconstruction archive");
}

std::filesystem::path prepare_pack(std::span<const uint8_t> rom,const std::filesystem::path& folder,
    const std::function<void(size_t)>& progress,std::stop_token shutdown) {
    if(rom.size()!=33554432 || XXH3_64bits(rom.data(),rom.size())!=0x44830C263C2DE108ULL)
        throw std::runtime_error("Reconstruction requires the validated JFG US revision0 ROM");
    const auto path=folder/"cache"/"textures"/"coordinate-v7.rtz";
    if(shutdown.stop_requested()) throw PreparationCancelled{};

    constexpr uint64_t expected_size=1970530095ULL,expected_hash=0x4cf50c0f03f077e8ULL;
    const auto digest=[&]() -> std::pair<uint64_t,uint64_t> {
        std::ifstream in(path,std::ios::binary|std::ios::ate);
        if(!in || in.tellg()<0 || uint64_t(in.tellg())>8ULL*1024*1024*1024) return {};
        const uint64_t size=uint64_t(in.tellg());in.seekg(0);
        XXH3_state_t state;XXH3_64bits_reset(&state);
        std::array<char,65536> buffer;
        while(in.read(buffer.data(),buffer.size()) || in.gcount()) {
            if(shutdown.stop_requested()) throw PreparationCancelled{};
            XXH3_64bits_update(&state,buffer.data(),size_t(in.gcount()));
        }
        if(!in.eof()) return {};
        return {size,XXH3_64bits_digest(&state)};
    };
    if(digest()==std::pair{expected_size,expected_hash}) {

        return path;
    }

    if(progress) progress(0);
    uint64_t published_size=0,published_hash=0;
    const auto result=files::write_atomic_stream(path,8ULL*1024*1024*1024,[&](FILE* file) {
        write_pack(rom,file,progress,shutdown);
        if(std::fflush(file)!=0 || std::fseek(file,0,SEEK_SET)!=0)
            throw std::runtime_error("Cannot verify reconstruction archive before publication");
        XXH3_state_t state;XXH3_64bits_reset(&state);
        std::array<uint8_t,65536> bytes;
        while(const size_t count=std::fread(bytes.data(),1,bytes.size(),file)) {
            if(shutdown.stop_requested()) throw PreparationCancelled{};
            published_size+=count;XXH3_64bits_update(&state,bytes.data(),count);
        }
        if(std::ferror(file)) throw std::runtime_error("Cannot read completed reconstruction archive");
        published_hash=XXH3_64bits_digest(&state);
        if(published_size!=expected_size || published_hash!=expected_hash)
            throw std::runtime_error("Generated reconstruction archive does not match the verified release identity");
    });
    if(!result.durable) std::fprintf(stderr,"JFC_TEXTURE_RECONSTRUCTION %s\n",result.warning.c_str());

    return path;
}
}
