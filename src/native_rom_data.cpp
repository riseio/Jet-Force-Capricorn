#include "native_rom_data.hpp"
#include "contrib/miniz/miniz.h"
#include "common/rt64_profiler_sha256.h"
#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace jfc::native_data {
namespace {
Definitions definitions;
bool initialized=false;
using Bytes=std::span<const uint8_t>;
Bytes extent(Bytes b,size_t p,size_t n) {
    if(p>b.size() || n>b.size()-p) throw std::runtime_error("Native definition extent outside input");
    return b.subspan(p,n);
}
uint32_t word(Bytes b,size_t p,unsigned width=4,bool little=false) {
    auto data=extent(b,p,width);
    uint32_t value=0;
    for(unsigned i=0;i<width;++i) value|=uint32_t(data[i])<<(8*(little?i:width-1-i));
    return value;
}
Bytes asset(Bytes rom,unsigned index) {
    if(index>=71) throw std::runtime_error("Invalid native asset index");
    const size_t first=word(rom,0xB1754+index*4),last=word(rom,0xB1754+(index+1)*4);
    if(first>last || last>rom.size()-0xB1880) throw std::runtime_error("Invalid native asset directory");
    return extent(rom,0xB1880+first,last-first);
}
Bytes indexed(Bytes offsets,Bytes data,unsigned index) {
    if(offsets.size()<8 || index>(offsets.size()-8)/4) throw std::runtime_error("Native definition index outside table: "+std::to_string(index)+" / "+std::to_string(offsets.size()/4));
    const size_t first=word(offsets,index*4),last=word(offsets,(index+1)*4);
    if(first>last) throw std::runtime_error("Invalid native indexed extent");
    return extent(data,first,last-first);
}
Bytes overlay(Bytes rom,unsigned number) {
    if (!number || number>157) throw std::runtime_error("Invalid definition overlay identity");
    const size_t entry=0x1ED2780+(number-1)*32;
    const uint64_t offset=uint64_t(0x1ED3B20)+word(rom,entry+4);
    const uint64_t length=uint64_t(word(rom,entry+8))+word(rom,entry+12);
    if (offset>rom.size() || length>rom.size()-offset) throw std::runtime_error("Invalid definition overlay extent");
    return extent(rom,size_t(offset),size_t(length));
}
unsigned constant(Bytes code,size_t site,unsigned destination=32) {
    const auto instruction=word(code,site);
    const unsigned target=(instruction>>16)&31;
    if ((instruction>>26)!=9 || ((instruction>>21)&31)!=0 || !target ||
        (destination<32 && target!=destination) || (instruction&0x8000))
        throw std::runtime_error("Invalid native reward constant instruction");
    return instruction&65535;
}
PickupMask game_flag(unsigned flag) {
    PickupMask result;

    if (flag>=32 && flag<44) result.parts=uint16_t(1U<<(flag-32));
    else if (flag>=44 && flag<47) result.floyd=uint8_t(1U<<(flag-44));
    return result;
}
void pickup_definitions(Bytes rom,Definitions& out) {
    const auto actor_index=asset(rom,48), object_index=asset(rom,46), objects=asset(rom,47);
    if (actor_index.size()%2 || actor_index.size()/2>65536) throw std::runtime_error("Invalid native actor inventory");
    out.actor_pickups.resize(actor_index.size()/2);
    const auto key_code=overlay(rom,108);
    const auto key_instruction=word(key_code,0x18);
    if ((key_instruction>>26)!=9 || ((key_instruction>>21)&31)==0 || int16_t(key_instruction)>=0)
        throw std::runtime_error("Invalid native key identifier origin");
    const unsigned key_origin=unsigned(-int(int16_t(key_instruction)));
    uint32_t key_mask=0;
    for (unsigned actor=0;actor<out.actor_pickups.size();++actor) {
        const unsigned index=word(actor_index,actor*2,2);

        if (index==out.actor_pickups.size()) continue;
        const auto definition=indexed(object_index,objects,index);
        if (definition.empty()) continue;
        const unsigned behavior=word(definition,0x1c,2);
        auto& entry=out.actor_pickups[actor];

        if (behavior==90) entry.selector=RewardSelector::Dialogue;
        else if (behavior==98) entry.selector=RewardSelector::Chest;
        else if (behavior==60) {
            if (actor<key_origin || actor-key_origin>=32) throw std::runtime_error("Invalid native key actor range");
            entry.direct.keys=1U<<(actor-key_origin); key_mask |= entry.direct.keys;
        }
    }
    const auto collectible=overlay(rom,43);
    size_t p=0x400;
    for (unsigned count=0;;++count,p+=32) {
        if (count>256) throw std::runtime_error("Unterminated native collectible table");
        const unsigned actor=word(collectible,p,2);
        if (actor==65535) break;
        extent(collectible,p,32);
        if (actor>=out.actor_pickups.size()) throw std::runtime_error("Invalid native collectible actor");
        if (word(collectible,p+4,1)==0)
            out.actor_pickups[actor].direct.merge(game_flag(word(collectible,p+6,1)));
    }
    const auto dialogue_code=overlay(rom,7);
    const unsigned action_count=word(dialogue_code,0x183c)&65535;
    if ((word(dialogue_code,0x183c)>>26)!=11 || !action_count || action_count>256)
        throw std::runtime_error("Invalid native dialogue action count");
    const auto action_table=extent(dialogue_code,0x2bc8,action_count*4);
    for (unsigned action=0;action<action_count;++action) {
        const size_t entry=word(action_table,action*4);
        if (entry%4 || entry>=dialogue_code.size()) throw std::runtime_error("Invalid native action target");
    }
    std::vector<PickupMask> actions(action_count);

    constexpr std::array<size_t,8> award_sites{0x186c,0x18c8,0x1b1c,0x1b60,0x1b88,0x1bb0,0x1bd8,0x1c8c};
    for (size_t site:award_sites) {
        const bool player=(site==award_sites[0] || site==award_sites[1]);
        const unsigned flag=constant(dialogue_code,site,player?6:4);
        PickupMask award;
        if (player) {
            if (flag>=32) throw std::runtime_error("Invalid native inventory award");
            award.keys=1U<<flag; key_mask |= award.keys;
        } else award=game_flag(flag);
        unsigned owner=action_count;
        size_t nearest=0;
        for (unsigned action=0;action<action_count;++action) {
            const size_t start=word(action_table,action*4);
            if (start<=site && (owner==action_count || start>nearest)) { nearest=start; owner=action; }
        }
        if (owner==action_count || site-nearest>32) throw std::runtime_error("Native award outside action block");
        actions[owner].merge(award);
    }
    const auto choices_index=asset(rom,9), choices_data=asset(rom,10), routes=asset(rom,8);
    if (choices_index.size()%4 || choices_index.size()<8 || choices_index.size()/4>65537)
        throw std::runtime_error("Invalid native dialogue index");
    const size_t choice_count=choices_index.size()/4-1;
    size_t route_position=0;

    for (unsigned selector=0;selector<out.dialogue_rewards.size() && route_position<routes.size();++selector) {
        const unsigned count=word(routes,route_position,2);
        const auto rows=extent(routes,route_position+2,size_t(count)*8);
        route_position+=2+rows.size();
        std::vector<uint8_t> visited(choice_count);
        std::vector<unsigned> pending;
        auto enqueue=[&](unsigned choice) {
            if (choice>=choice_count) throw std::runtime_error("Native dialogue choice outside table");
            if (!visited[choice]) { visited[choice]=1; pending.push_back(choice); }
        };
        for (unsigned row=0;row<count;++row)
            if (word(rows,row*8+3,1)==1) enqueue(word(rows,row*8+6,2));
        while (!pending.empty()) {
            const unsigned choice=pending.back(); pending.pop_back();
            const auto blob=indexed(choices_index,choices_data,choice);
            const unsigned n=word(blob,0,2);
            const auto records=extent(blob,4,size_t(n)*16);
            for (unsigned i=0;i<n;++i) {
                const unsigned action=word(records,i*16+8,2);
                if (action<actions.size()) out.dialogue_rewards[selector].merge(actions[action]);
                else if ((action&0xf000)==0x2000) enqueue(action&0xfff);
            }
        }
    }
    if (route_position!=routes.size()) throw std::runtime_error("Excess native dialogue routes");
    const auto chest=overlay(rom,82);
    const auto range=word(chest,0xfd8);
    const unsigned chest_count=range&65535;
    if ((range>>26)!=11 || !chest_count || chest_count>out.chest_rewards.size()) throw std::runtime_error("Invalid native chest selector range");
    for (unsigned selector=0;selector<chest_count;++selector) {
        const size_t block=word(chest,0x1844+selector*4);
        const auto record=extent(chest,block,32);
        const unsigned flag=constant(record,0), reg=(word(record,0)>>16)&31;
        const auto store=word(record,12), tail=word(record,24);
        const auto mode=(tail>>26)==9 ? tail : word(record,28);
        if ((store>>26)!=43 || ((store>>21)&31)!=29 || ((store>>16)&31)!=reg || (store&65535)!=0x4c ||
            ((tail>>26)!=9 && (tail>>16)!=0x1000) || (mode>>26)!=9 || ((mode>>21)&31)!=0)
            throw std::runtime_error("Invalid native chest award record: "+std::to_string(selector));
        if (((mode>>16)&31)==4 && flag<32 && (key_mask&(1U<<flag)))
            out.chest_rewards[selector].keys=1U<<flag;
    }
    const auto level=indexed(asset(rom,30),asset(rom,31),394);
    const auto script=indexed(asset(rom,65),asset(rom,66),word(level,0xe8,2));
    if (word(script,56,4)!=0x05147a40 || word(script,62,4)!=0x05147801)
        throw std::runtime_error("Invalid native event reward commands");
    out.event_level=uint16_t(word(script,60,2));
    out.event_reward=game_flag(word(script,66,2));
    uint32_t placed_keys=0;
    const auto placements_index=asset(rom,28), placements_data=asset(rom,29);
    if (placements_index.size()%4) throw std::runtime_error("Invalid native placement index");
    for (size_t i=0;i+8<=placements_index.size();i+=4) {
        const size_t first=word(placements_index,i),last=word(placements_index,i+4);
        if (first==UINT32_MAX || last==UINT32_MAX || first==last) continue;
        if (first>last) throw std::runtime_error("Invalid native placement extent");
        const auto blob=extent(placements_data,first,last-first);
        const auto records=extent(blob,16,word(blob,0));
        for (size_t at=0;at<records.size();) {
            const unsigned actor=word(records,at,2), size=word(records,at+2,1);
            if (size<10 || size&1) throw std::runtime_error("Invalid native placement size");
            const auto record=extent(records,at,size);
            if (actor<out.actor_pickups.size()) {
                const auto& pickup=out.actor_pickups[actor];
                placed_keys |= pickup.direct.keys;
                if (pickup.selector==RewardSelector::Dialogue) placed_keys |= out.dialogue_rewards[word(record,0xe,1)].keys;
                else if (pickup.selector==RewardSelector::Chest) placed_keys |= out.chest_rewards[word(record,0xa,1)].keys;
            }
            at+=size;
        }
    }
    const auto inventory=overlay(rom,18);
    p=0x6938;
    for (unsigned count=0;;++count) {
        if (count>128) throw std::runtime_error("Unterminated native inventory layout");
        const unsigned marker=word(inventory,p,2);
        if (marker==65535) break;
        if (marker==0) p+=8;
        const auto row=extent(inventory,p,10); p+=10;
        const unsigned flag=word(row,6,2),variant=word(row,0,2);
        if (flag<32 && (placed_keys&(1U<<flag))) {
            if (variant>=out.key_bounds.size()) throw std::runtime_error("Invalid native key model variant");
            out.key_icons.push_back({flag,int(variant)});
            placed_keys &= ~(1U<<flag);
        }
    }
    if (placed_keys) throw std::runtime_error("Native placed key lacks inventory model");
}
IconBounds icon_bounds(Bytes rom,unsigned item,unsigned variant) {
    const unsigned actor=word(asset(rom,26),item*2,2)&0x3fff;
    const unsigned definition=word(asset(rom,48),actor*2,2);
    const auto object=indexed(asset(rom,46),asset(rom,47),definition);
    if(variant>=word(object,0x1f,1)) throw std::runtime_error("Invalid native model variant");
    const unsigned model=word(object,size_t(word(object,0x30))+variant*4);
    const auto compressed=indexed(asset(rom,38),asset(rom,39),model);
    const unsigned size=word(compressed,0,4,true);
    if(size<32 || size>1024*1024) throw std::runtime_error("Invalid native model inflate size");
    const auto stream=extent(compressed,5,compressed.size()-std::min(size_t(5),compressed.size()));
    std::vector<uint8_t> decoded(size);
    if(tinfl_decompress_mem_to_mem(decoded.data(),decoded.size(),stream.data(),stream.size(),0)!=size)
        throw std::runtime_error("Invalid native model deflate stream");
    const unsigned count=word(decoded,0x12,2),position=word(decoded,0x1c);
    if(!count) throw std::runtime_error("Empty native icon model");
    const auto vertices=extent(decoded,position,size_t(count)*10);
    IconBounds bounds{INT16_MAX,INT16_MIN,INT16_MAX,INT16_MIN};
    for(unsigned i=0;i<count;++i) {
        const int x=int16_t(word(vertices,i*10,2)),y=int16_t(word(vertices,i*10+2,2));
        bounds.left=std::min(bounds.left,x); bounds.right=std::max(bounds.right,x);
        bounds.bottom=std::min(bounds.bottom,y); bounds.top=std::max(bounds.top,y);
    }
    if(bounds.left==bounds.right || bounds.bottom==bounds.top) throw std::runtime_error("Degenerate native icon model");
    return bounds;
}
}
void initialize(std::span<const uint8_t> rom) {
    if (rom.size()!=33554432) throw std::runtime_error("Invalid native definition ROM size");
    constexpr std::array<uint32_t,12> sites{
        0x1FDB070,0x1FDAFFC,0x1FDAE44,0x1FDAF20,
        0x1FDF6B4,0x1FDF5E4,0x1FDF64C,0x1FDF57C,
        0x1FDF440,0x1FDF3C4,0x1FDF72C,0x1FDF4CC};
    Definitions candidate;
    for(size_t i=0;i<sites.size();++i) {
        const size_t p=sites[i];
        if (p>rom.size()-4) throw std::runtime_error("Native boss definition outside ROM");
        const uint32_t word=uint32_t(rom[p])<<24|uint32_t(rom[p+1])<<16|uint32_t(rom[p+2])<<8|rom[p+3];
        const int threshold=int16_t(word&0xffff);
        if ((word>>26)!=10 || threshold<=0) throw std::runtime_error("Invalid native boss threshold instruction");
        candidate.boss_thresholds[i]=threshold;
    }

    constexpr std::array<size_t,4> mantis_sites{0x1F8889C,0x1F89A08,0x1F89A6C,0x1F89AD0};
    int initial_budget = 0;
    for (size_t i=0; i<mantis_sites.size(); ++i) {
        const uint32_t instruction = word(rom,mantis_sites[i]);
        const int budget = int16_t(instruction & 0xffff);
        if ((instruction >> 26) != 9 || ((instruction >> 21) & 31) != 0 ||
            ((instruction >> 16) & 31) == 0 || budget <= 0)
            throw std::runtime_error("Invalid native Mantis budget instruction");
        if (i == 0) initial_budget = budget;
        else candidate.mantis_phase_budgets[i-1] = budget;
    }
    if (initial_budget != candidate.mantis_phase_budgets[0])
        throw std::runtime_error("Inconsistent native Mantis initial and reset budgets");
    constexpr size_t message=12808953, maximum=64;
    if (message>rom.size()-maximum) throw std::runtime_error("Native diagnostic label outside ROM");
    for(size_t i=0;i<maximum;++i) {
        const unsigned char c=rom[message+i];
        if (!c) break;
        if (c<32 || c>126) throw std::runtime_error("Invalid native diagnostic label");
        candidate.pickup_capacity_label.push_back(char(c));
    }
    if (candidate.pickup_capacity_label.empty() || candidate.pickup_capacity_label.size()==maximum)
        throw std::runtime_error("Unterminated native diagnostic label");

    const auto dialogue = indexed(asset(rom,9),asset(rom,10),57);
    const auto clause = extent(dialogue,2001,candidate.tribal_requirement_clause.size());
    RT64::ProfilerDetail::Sha256 clause_hash;
    clause_hash.update(clause.data(),clause.size());
    if (clause_hash.hex() != "920f9b4b12e2d99b367ce308c139a4d889e3cb607a654314145b482597811645")
        throw std::runtime_error("Unsupported native Tribal dialogue span");
    std::copy(clause.begin(),clause.end(),candidate.tribal_requirement_clause.begin());

    for(unsigned i=0;i<candidate.part_bounds.size();++i) candidate.part_bounds[i]=icon_bounds(rom,111+i,0);
    for(unsigned i=0;i<candidate.key_bounds.size();++i) candidate.key_bounds[i]=icon_bounds(rom,145,i);
    candidate.token_bounds=icon_bounds(rom,160,0);
    candidate.floyd_bounds=icon_bounds(rom,160,1);
    for(unsigned i=2;i<=4;++i) {
        const auto b=icon_bounds(rom,160,i);
        auto& combined=candidate.floyd_bounds;
        combined.left=std::min(combined.left,b.left); combined.right=std::max(combined.right,b.right);
        combined.bottom=std::min(combined.bottom,b.bottom); combined.top=std::max(combined.top,b.top);
    }
    pickup_definitions(rom,candidate);
    definitions=std::move(candidate);
    initialized=true;
}
const Definitions& get() {
    if (!initialized) throw std::logic_error("Native ROM definitions are not initialized");
    return definitions;
}
}
