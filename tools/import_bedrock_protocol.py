"""Compatibility CLI: build a header from an explicitly selected local data source."""
from embed_protocol import generate
from pathlib import Path
import argparse
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--version-source',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();generate(args.source,args.version_source,Path(__file__).resolve().parents[1]/'data/policies/movement.json',args.output)
