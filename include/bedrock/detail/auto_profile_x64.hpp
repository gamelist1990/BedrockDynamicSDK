#pragma once
#include <bedrock/auto_profile.hpp>
#include <hde64.h>
#include <array>

namespace bedrock::auto_profile {
// Use instruction lengths, not guessed byte positions. Unsupported VEX/EVEX
// or truncated instructions terminate a window; they are never decoded as data.
// HDE may look past 15 bytes before rejecting malformed prefix sequences.
// Keep decoder lookahead inside owned padding; accept only original input bytes.
inline std::string instructionSignature(std::span<const std::byte> input){
    const char* digits="0123456789ABCDEF";std::string out;
    for(std::size_t offset=0;offset<input.size();){
        std::array<std::byte,64> padded{};const auto count=std::min<std::size_t>(16,input.size()-offset);
        std::memcpy(padded.data(),input.data()+offset,count);hde64s h{};
        if(!hde64_disasm(padded.data(),&h)||h.flags&F_ERROR||!h.len||h.len>count||h.opcode==0xC4||h.opcode==0xC5||h.opcode==0x62)break;
        const auto immediateFlags=h.flags&(F_IMM8|F_IMM16|F_IMM32|F_IMM64);if(immediateFlags&&(immediateFlags&(immediateFlags-1)))break;
        const unsigned imm=(h.flags&F_IMM64)?8:(h.flags&F_IMM32)?4:(h.flags&F_IMM16)?2:(h.flags&F_IMM8)?1:0;
        const unsigned disp=(h.flags&F_DISP32)?4:(h.flags&F_DISP16)?2:(h.flags&F_DISP8)?1:0;
        if(imm+disp>h.len)break;
        for(unsigned i=0;i<h.len;++i){if(!out.empty())out+=' ';
            if(i>=h.len-imm-disp)out+="??";
            else{const auto value=std::to_integer<unsigned>(input[offset+i]);out+=digits[value>>4];out+=digits[value&15];}}
        offset+=h.len;
        if(h.opcode==0xC3||h.opcode==0xC2||h.opcode==0xCC||h.opcode==0xE9||h.opcode==0xEB||(h.opcode==0xFF&&(h.modrm_reg==4||h.modrm_reg==5)))break;
    }
    return out;
}
// Facts describe decoded operands, not named C++ members or invariants.
inline Json instructionFacts(std::span<const std::byte> input){
    Json facts=Json::array();std::size_t offset{};bool terminal=false;
    for(unsigned count=0;offset<input.size()&&count<256;++count){
        std::array<std::byte,64> padded{};const auto available=std::min<std::size_t>(16,input.size()-offset);
        std::memcpy(padded.data(),input.data()+offset,available);hde64s h{};
        if(!hde64_disasm(padded.data(),&h)||h.flags&F_ERROR||!h.len||h.len>available||h.opcode==0xC4||h.opcode==0xC5||h.opcode==0x62)break;
        const auto immediateFlags=h.flags&(F_IMM8|F_IMM16|F_IMM32|F_IMM64);if(immediateFlags&&(immediateFlags&(immediateFlags-1)))break;
        Json value={{"offset",offset},{"length",h.len},{"opcode",hex(h.opcode)},{"relative",bool(h.flags&F_RELATIVE)}};
        if(h.flags&(F_DISP8|F_DISP16|F_DISP32)){
            value["displacement"]=(h.flags&F_DISP32)?static_cast<std::int64_t>(static_cast<std::int32_t>(h.disp.disp32)):
                (h.flags&F_DISP16)?static_cast<std::int64_t>(static_cast<std::int16_t>(h.disp.disp16)):static_cast<std::int64_t>(static_cast<std::int8_t>(h.disp.disp8));
            value["rip_relative"]=h.modrm_mod==0&&h.modrm_rm==5;value["modrm_rm"]=h.modrm_rm;value["rex_b"]=h.rex_b;
        }
        if(h.flags&(F_IMM8|F_IMM16|F_IMM32|F_IMM64))value["immediate"]=(h.flags&F_IMM64)?std::uint64_t(h.imm.imm64):(h.flags&F_IMM32)?std::uint64_t(h.imm.imm32):(h.flags&F_IMM16)?std::uint64_t(h.imm.imm16):std::uint64_t(h.imm.imm8);
        if(value.contains("displacement")||value.contains("immediate"))facts.push_back(std::move(value));
        offset+=h.len;if(h.opcode==0xC3||h.opcode==0xC2||h.opcode==0xCC||h.opcode==0xE9||h.opcode==0xEB||(h.opcode==0xFF&&(h.modrm_reg==4||h.modrm_reg==5))){terminal=true;break;}
    }
    Json result={{"operands",facts},{"decoded_prefix_bytes",offset},{"terminal",terminal},{"maximum_instructions",256},{"scope","instruction operand facts; not established object offsets, meanings or immutable constants"}};
    if(input.size()>=8&&input[0]==std::byte{0x48}&&input[1]==std::byte{0x8B}&&input[2]==std::byte{0x81}&&input[7]==std::byte{0xC3}){
        std::uint32_t displacement{};std::memcpy(&displacement,input.data()+3,4);
        result["pointer_getter"]={{"base","RCX"},{"displacement",displacement},{"bytes",8},{"instruction_shape","mov rax,[rcx+disp32]; ret"}};
    }
    return result;
}
} // namespace bedrock::auto_profile
