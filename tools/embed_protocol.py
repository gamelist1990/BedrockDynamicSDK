"""Build-time protocol header from the one complete data/protocols source."""
from pathlib import Path
import argparse, hashlib, json


def generate(source: Path, version_source: Path, policy_source: Path, output: Path):
    raw=source.read_bytes();text=raw.decode('utf-8');types=json.loads(text)['types']
    version=json.loads(version_source.read_text(encoding='utf-8'))
    ids={name:int(number) for number,name in types['mcpe_packet'][1][0]['type'][1]['mappings'].items()}
    flags={name:int(number) for number,name in types['InputData'][1]['mappings'].items()}
    camel=lambda name: ''.join(part.title() for part in name.split('_'))
    lines=['#pragma once','#include <array>','#include <cstdint>','#include <string_view>',
           '// Generated at build time from data. Do not edit or commit this header.',
           'namespace bedrock::protocol {',f'inline constexpr unsigned protocolVersion={version["version"]};']
    lines+=['enum class PacketType : std::uint16_t {']+[f'    {camel(name)}={value},' for name,value in ids.items()]+['};']
    lines+=['constexpr std::string_view packetName(std::uint16_t id) noexcept {','    switch(id){']+[f'    case {value}:return "{name}";' for name,value in ids.items()]+['    default:return "unknown";','    }','}']
    lines+=['enum class InputData : unsigned {']+[f'    {camel(name)}={index},' for name,index in flags.items()]+['};']
    movement=json.loads(policy_source.read_text(encoding='utf-8'))['controls']
    lines+=['inline constexpr std::array movementControls{']+['    InputData::'+camel(name)+',' for name in movement if name in flags]+['};',
        'constexpr void clearMovement(std::array<std::uint64_t,2>& flags) noexcept {',
        '    for(auto flag:movementControls){const auto bit=static_cast<unsigned>(flag); flags[bit/64]&=~(std::uint64_t{1}<<(bit%64));}','}',
        'constexpr bool has(const std::array<std::uint64_t,2>& flags,InputData flag) noexcept {',
        '    const auto bit=static_cast<unsigned>(flag);return (flags[bit/64]&(std::uint64_t{1}<<(bit%64)))!=0;','}']
    chunks=[text[i:i+12000] for i in range(0,len(text),12000)]
    if ')BEDROCK_SCHEMA"' in text:raise ValueError('schema delimiter collision')
    lines+=['inline constexpr std::array<std::string_view,'+str(len(chunks))+'> sourceChunks{']+['R"BEDROCK_SCHEMA('+chunk+')BEDROCK_SCHEMA",' for chunk in chunks]+['};',
       'inline constexpr std::string_view sourceSha256="'+hashlib.sha256(raw).hexdigest()+'";',
       'inline constexpr std::string_view minecraftVersion="'+version['minecraftVersion']+'";',
       '} // namespace bedrock::protocol','']
    output.parent.mkdir(parents=True,exist_ok=True);output.write_text('\n'.join(lines),encoding='utf-8')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--source',type=Path,required=True);parser.add_argument('--version-source',type=Path,required=True)
    parser.add_argument('--policy',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    generate(args.source,args.version_source,args.policy,args.output)
