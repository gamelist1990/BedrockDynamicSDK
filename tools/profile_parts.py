"""Split profiles, assemble build artifacts, and refresh each part checksum."""
from pathlib import Path
import argparse,hashlib,json
from bedrock_symbols.profile_parts import load_profile,split_profile,write_json

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('operation',choices=['split','assemble','refresh']);p.add_argument('path',type=Path);p.add_argument('--output',type=Path);args=p.parse_args()
    if args.operation=='split':split_profile(args.path)
    elif args.operation=='assemble':
        if not args.output:p.error('--output required')
        write_json(args.output,load_profile(args.path))
    else:
        root=json.loads(args.path.read_text(encoding='utf-8'))
        for part in root['parts']:part['sha256']=hashlib.sha256((args.path.parent/part['path']).read_bytes()).hexdigest()
        write_json(args.path,root)
