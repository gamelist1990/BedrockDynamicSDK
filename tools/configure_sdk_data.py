"""Configure-time embedding from the manifest; no version names in SDK code."""
from pathlib import Path
import argparse,json,hashlib
from bedrock_symbols.profile_parts import load_profile
from bedrock_symbols.embed import make_header
from embed_protocol import generate

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--root',type=Path,required=True);p.add_argument('--version',default='');p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    root=args.root;manifest=json.loads((root/'data/manifest.json').read_text(encoding='utf-8'))
    version=args.version or manifest['default_version'];entry=manifest['versions'][version]['windows-x64']
    source=root/entry['path'];profile=load_profile(source);protocol=profile['protocol']
    protocol_path=(root/protocol['path']).resolve();version_path=(root/protocol['version_path']).resolve()
    if not protocol_path.is_relative_to((root/'data').resolve()) or not version_path.is_relative_to((root/'data').resolve()):raise ValueError('protocol path outside data')
    if hashlib.sha256(protocol_path.read_bytes()).hexdigest()!=protocol['sha256']:raise ValueError('protocol source hash mismatch')
    make_header(source,args.output/'bedrock/generated/symbols.hpp')
    generate(protocol_path,version_path,root/'data/policies/movement.json',args.output/'bedrock/generated/protocol.hpp')
    # CheckSig references are all data-defined versions, not old/new C++ names.
    refs=[]
    for version,platforms in sorted(manifest['versions'].items()):
        value=load_profile(root/platforms['windows-x64']['path']);value.pop('analysis',None);refs.append(json.dumps(value,ensure_ascii=False,separators=(',',':')))
    header=['#pragma once','#include <array>','#include <string_view>','#include <string>','namespace bedrock::generated {',f'inline const std::array<std::string,{len(refs)}> profileSources{{']
    for text in refs:
        if ')BDK_AUTO"' in text:raise ValueError('profile delimiter collision')
        header.append('[]{std::string value;')
        for offset in range(0,len(text),12000):header.append('value.append(R"BDK_AUTO('+text[offset:offset+12000]+')BDK_AUTO");')
        header.append('return value;}(),')
    header+=['};','}'];(args.output/'bedrock/generated/profile_sources.hpp').write_text('\n'.join(header)+'\n',encoding='utf-8')
