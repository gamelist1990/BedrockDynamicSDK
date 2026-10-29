"""Canonical class-based profiles and deterministic compatibility assembly."""
from pathlib import Path
import hashlib,json,re

FORMAT='bedrock-profile-parts'

def merge(root,part):
    for key,value in part.items():
        if key in {'symbols','layouts','virtual_methods','analysis'}:
            target=root.setdefault(key,{})
            if not isinstance(value,dict):raise ValueError('profile section must be an object: '+key)
            for name,definition in value.items():
                if name in target:raise ValueError('duplicate profile definition: '+key+'/'+name)
                target[name]=definition
        elif key in root:raise ValueError('duplicate profile metadata: '+key)
        else:root[key]=value

def load_profile(path:Path):
    path=Path(path)
    if not path.exists() and path.name=='windows-x64.json':path=path.with_name('windows-x64')/'manifest.json'
    value=json.loads(path.read_text(encoding='utf-8'))
    if value.get('format')!=FORMAT:return value
    root={key:value[key] for key in ['schema','target','protocol'] if key in value}
    if not isinstance(value['parts'],list) or len(value['parts'])>256:raise ValueError('invalid profile parts')
    base=path.parent.resolve();total=0
    for entry in value['parts']:
        relative=entry['path']
        if not re.fullmatch(r'(classes|analysis|metadata)/[A-Za-z0-9_.-]+\.json',relative):raise ValueError('invalid part path')
        part_path=(base/relative).resolve()
        if not part_path.is_relative_to(base):raise ValueError('profile part escapes root')
        raw=part_path.read_bytes();total+=len(raw)
        if len(raw)>4*1024*1024 or total>16*1024*1024:raise ValueError('profile byte limit')
        if hashlib.sha256(raw).hexdigest()!=entry['sha256']:raise ValueError('profile part checksum mismatch: '+relative)
        merge(root,json.loads(raw))
    root.setdefault('symbols',{})
    return root

def write_json(path,value):
    path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

def split_profile(source:Path):
    value=load_profile(source);base=source.with_name('windows-x64');parts={}
    def put(path,section,key,definition):parts.setdefault(path,{}).setdefault(section,{})[key]=definition
    for section in ['layouts','symbols','virtual_methods']:
        for name,definition in value.get(section,{}).items():
            owner=name if section=='layouts' else name.split('::')[0]
            if owner.startswith('layouts.'):owner=owner.split('.')[1]
            if owner.startswith('sub_'):owner='Internal'
            owner=re.sub('[^A-Za-z0-9_.-]','_',owner)
            put('classes/'+owner+'.json',section,name,definition)
    for key,definition in value.get('analysis',{}).items():put('analysis/'+key+'.json','analysis',key,definition)
    for key,definition in value.items():
        if key not in {'schema','target','protocol','layouts','symbols','virtual_methods','analysis'}:parts['metadata/'+key+'.json']={key:definition}
    entries=[]
    for relative,part in sorted(parts.items()):
        path=base/relative;write_json(path,part);entries.append({'path':relative,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
    descriptor={key:value[key] for key in ['schema','target','protocol'] if key in value};descriptor['format']=FORMAT;descriptor['parts']=entries
    write_json(base/'manifest.json',descriptor)
    return base/'manifest.json'
