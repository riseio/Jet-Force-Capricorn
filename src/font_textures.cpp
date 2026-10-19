#include "font_textures.hpp"
#include "texture_bank.hpp"
#include "atomic_file.hpp"
#include "font_magnification.hpp"
#include "contrib/miniz/miniz.h"
#define XXH_INLINE_ALL
#include "contrib/xxhash/xxhash.h"
#include <algorithm>
#include <array>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>

namespace jfc::textures {
Image reconstruct_font(const Image& source) {
    if (!source.width || !source.height || source.width > 16 || source.height > 16 ||
            source.rgba.size() != size_t(source.width) * source.height * 4)
        throw std::runtime_error("Invalid native font image");
    Image contour{source.width*2,source.height*2,{}};
    contour.rgba.resize(size_t(contour.width)*contour.height*4);

    for (unsigned y=0;y<source.height;++y) for (unsigned x=0;x<source.width;++x) {
        for (unsigned channel : {0U,3U}) {
            const auto sample=[&](int sx,int sy) {
                return int(source.rgba[(size_t(std::clamp(sy,0,int(source.height)-1))*source.width+
                    unsigned(std::clamp(sx,0,int(source.width)-1)))*4+channel]);
            };
            const auto values=detail::magnify(int(x),int(y),sample);
            for (unsigned j=0;j<2;++j) for (unsigned i=0;i<2;++i) {
                const size_t p=(size_t(y*2+j)*contour.width+x*2+i)*4;
                contour.rgba[p+channel]=uint8_t(values[j*2+i]);
                if (!channel) contour.rgba[p+1]=contour.rgba[p+2]=contour.rgba[p];
            }
        }
    }

    return reconstruct(contour,4);
}

Image reconstruct_number_sheet(const Image& source, uint32_t cell_width) {
    if (!cell_width || cell_width>16 || source.width<cell_width ||
            source.width>256 || !source.height || source.height>16 ||
            source.rgba.size()!=size_t(source.width)*source.height*4)
        throw std::runtime_error("Invalid native number sheet dimensions");
    Image result{source.width*8,source.height*8,{}};
    result.rgba.resize(size_t(result.width)*result.height*4);

    for (unsigned left=0; left<source.width; left+=cell_width) {
        Image cell{std::min(cell_width,source.width-left),source.height,{}};
        cell.rgba.resize(size_t(cell.width)*cell.height*4);
        for (unsigned y=0;y<cell.height;++y)
            std::copy_n(source.rgba.begin()+(size_t(y)*source.width+left)*4,
                cell.width*4,cell.rgba.begin()+size_t(y)*cell.width*4);
        const auto expanded=reconstruct_font(cell);
        for (unsigned y=0;y<expanded.height;++y)
            std::copy_n(expanded.rgba.begin()+size_t(y)*expanded.width*4,
                expanded.width*4,result.rgba.begin()+(size_t(y)*result.width+left*8)*4);
    }
    return result;
}

std::vector<uint8_t> make_font_pack(std::span<const uint8_t> rom) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip,0,1024*1024))
        throw std::runtime_error("Cannot allocate native font archive");
    struct Owner {mz_zip_archive* zip;~Owner(){mz_zip_writer_end(zip);}} owner{&zip};
    std::tm calendar{};calendar.tm_year=80;calendar.tm_mday=1;calendar.tm_isdst=-1;
    MZ_TIME_T stamp=std::mktime(&calendar);
    if (stamp==MZ_TIME_T(-1)) throw std::runtime_error("Cannot represent font archive timestamp");
    const auto add=[&](const std::string& name,std::span<const uint8_t> bytes) {
        if(!mz_zip_writer_add_mem_ex_v2(&zip,name.c_str(),bytes.data(),bytes.size(),nullptr,0,6,0,0,&stamp,nullptr,0,nullptr,0,8))
            throw std::runtime_error("Cannot write native font archive");
    };
    std::map<uint64_t,Image> originals;
    std::string metadata=R"({"configuration":{"autoPath":"rt64","configurationVersion":3,"hashVersion":6,"defaultOperation":"preload","defaultShift":"half"},"textures":[)";
    visit_font_tiles(rom,[&](FontTile&& tile) {
        const auto [it,inserted]=originals.try_emplace(tile.hash,std::move(tile.image));
        if(!inserted) {
            if(it->second.width!=tile.image.width || it->second.height!=tile.image.height || it->second.rgba!=tile.image.rgba)
                throw std::runtime_error("Native font hash aliases different artwork");
            return;
        }
        char hash[17];std::snprintf(hash,sizeof(hash),"%016llx",static_cast<unsigned long long>(tile.hash));
        if(originals.size()!=1) metadata+=',';
        metadata+=std::string("{\"hashes\":{\"rt64\":\"")+hash+"\"},\"path\":\""+hash+"\"}";
        add(std::string(hash)+".dds",make_dds(reconstruct_font(it->second)));
    });
    visit_number_tiles(rom,[&](BankTile&& tile,uint32_t width) {
        if (originals.contains(tile.hash))
            throw std::runtime_error("Number sheet aliases a native font cell");
        char hash[17];std::snprintf(hash,sizeof(hash),"%016llx",static_cast<unsigned long long>(tile.hash));
        metadata+=std::string(",{\"hashes\":{\"rt64\":\"")+hash+"\"},\"path\":\""+hash+
            "\",\"horizontalCellWidth\":"+std::to_string(width)+"}";
        add(std::string(hash)+".dds",make_dds(reconstruct_number_sheet(tile.image,width)));
    });
    metadata+="]}";
    add("rt64.json",{reinterpret_cast<const uint8_t*>(metadata.data()),metadata.size()});
    void* data=nullptr;size_t size=0;
    if(!mz_zip_writer_finalize_heap_archive(&zip,&data,&size))
        throw std::runtime_error("Cannot finalize native font archive");
    std::unique_ptr<void,decltype(&mz_free)> memory(data,mz_free);
    return {static_cast<uint8_t*>(data),static_cast<uint8_t*>(data)+size};
}

std::filesystem::path prepare_font_pack(std::span<const uint8_t> rom,const std::filesystem::path& folder) {
    if(rom.size()!=33554432 || XXH3_64bits(rom.data(),rom.size())!=0x44830C263C2DE108ULL)
        throw std::runtime_error("Font textures require validated JFG US revision0 ROM");

    constexpr size_t pack_size=3094856;
    constexpr uint64_t pack_hash=0x673205EC3C6F6E3EULL;
    const auto directory=folder/"cache"/"textures";
    const auto path=directory/"native-fonts-v2.rtz";
    for(const auto& p : {folder/"cache",directory,path}) {
        std::error_code error;
        const auto status=std::filesystem::symlink_status(p,error);
        if(error && error!=std::errc::no_such_file_or_directory)
            throw std::filesystem::filesystem_error("Cannot inspect font cache",p,error);
        if(std::filesystem::is_symlink(status) || (std::filesystem::exists(status) &&
                !(p==path?std::filesystem::is_regular_file(status):std::filesystem::is_directory(status))))
            throw std::runtime_error("Native font cache must use regular files and directories");
    }
    std::error_code error;
    if(std::filesystem::file_size(path,error)==pack_size && !error) {
        std::vector<uint8_t> existing(pack_size);
        std::ifstream input(path,std::ios::binary);
        if(input.read(reinterpret_cast<char*>(existing.data()),std::streamsize(existing.size())) &&
                XXH3_64bits(existing.data(),existing.size())==pack_hash)
            return path;
    }
    const auto bytes=make_font_pack(rom);
    if(bytes.size()!=pack_size || XXH3_64bits(bytes.data(),bytes.size())!=pack_hash)
        throw std::runtime_error("Native font reconstruction differs from the verified recipe");
    std::filesystem::create_directories(directory);
    const auto result=files::write_atomic(path,bytes);
    if(!result.durable) std::fprintf(stderr,"Native fonts: %s\n",result.warning.c_str());
    return path;
}
}
