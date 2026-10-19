#include "scalar_shader.hpp"
#include <stdexcept>

namespace jfc::shader {
std::string scalar_block(std::span<const uint8_t> bytes, std::span<const CodeSpan> spans,
    const std::array<std::string,32>& inputs, unsigned output) {
    if (output >= inputs.size() || spans.empty() || spans.size() > 32)
        throw std::invalid_argument("Invalid scalar shader block metadata");
    auto values = inputs;
    values[0] = "0u";
    std::string source;
    size_t instructions = 0;
    const auto read = [&](unsigned r) -> const std::string& {
        if (values[r].empty()) throw std::runtime_error("Scalar shader reads an undefined register");
        return values[r];
    };

    for (auto span : spans) {
        if (!span.size || (span.offset|span.size)&3 || span.offset > bytes.size() ||
            span.size > bytes.size()-span.offset || span.size/4 > 256-instructions)
            throw std::runtime_error("Scalar shader block exceeds input bounds");
        for (size_t p=span.offset; p<span.offset+span.size; p+=4) {
            const uint32_t word = uint32_t(bytes[p])<<24 | uint32_t(bytes[p+1])<<16 |
                uint32_t(bytes[p+2])<<8 | bytes[p+3];
            const unsigned opcode=word>>26, rs=(word>>21)&31, rt=(word>>16)&31,
                rd=(word>>11)&31, shift=(word>>6)&31, function=word&63;
            unsigned destination=rt;
            std::string expression;
            if (opcode==12 || opcode==13 || opcode==14) {
                expression=read(rs)+(opcode==12?" & ":opcode==13?" | ":" ^ ")+
                    std::to_string(word&65535)+"u";
            } else if (opcode==0) {
                destination=rd;
                if ((function==0 || function==2) && rs==0)
                    expression=read(rt)+(function==0?" << ":" >> ")+std::to_string(shift)+"u";
                else if ((function==4 || function==6) && shift==0)
                    expression=read(rt)+(function==4?" << (":" >> (")+read(rs)+" & 31u)";
                else if (shift==0 && (function==32 || function==33 || function==34 || function==35 ||
                         function==36 || function==37 || function==38)) {
                    const char* op=function<=33?" + ":function<=35?" - ":function==36?" & ":function==37?" | ":" ^ ";
                    expression=read(rs)+op+read(rt);
                }
            }
            if (expression.empty()) throw std::runtime_error("Unsupported scalar shader instruction");
            const auto name="v"+std::to_string(instructions++);
            source+="    uint "+name+" = "+expression+";\n";
            if (destination) values[destination]=name;
        }
    }
    source+="    return "+read(output)+";\n";
    return source;
}

std::string cpu_effect_source(std::span<const uint8_t> rom, std::string_view source_template) {
    if (rom.size()!=33554432) throw std::runtime_error("Invalid shader ROM size");

    std::array<std::string,32> inputs{};
    inputs[8]="value"; inputs[6]="(operation >> 16u)";
    inputs[15]="operation"; inputs[24]="operation";
    constexpr std::array spans{CodeSpan{0x24258,4},CodeSpan{0x24260,4},CodeSpan{0x2437C,56}};
    const auto blend=scalar_block(rom,spans,inputs,8);
    inputs={}; inputs[15]="(operation & 65535u)";
    constexpr std::array opaque_span{CodeSpan{0x24340,4}};
    const auto opaque=scalar_block(rom,opaque_span,inputs,15);
    std::string source(source_template);
    for (const auto& replacement : {std::pair{"JFC_NATIVE_BLEND",blend},std::pair{"JFC_NATIVE_OPAQUE",opaque}}) {
        const std::string_view marker=replacement.first;
        const size_t at=source.find(marker);
        if (at==std::string::npos || source.find(marker,at+marker.size())!=std::string::npos)
            throw std::runtime_error("Invalid native shader template marker");
        source.replace(at,marker.size(),replacement.second);
    }
    return source;
}
}
