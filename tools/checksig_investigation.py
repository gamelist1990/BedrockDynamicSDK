"""Generate the source-backed investigation catalog and x64 CheckSig handoff files."""
from bedrock_symbols.profile_parts import load_profile
from pathlib import Path
import argparse,json,re,shutil,zipfile
ROOT=Path(__file__).resolve().parents[1]
VERSION='1.26.60.5'
OLD='1.26.52.3'

def generate_catalog():
 old=load_profile(ROOT/f'data/{OLD}/windows-x64.json')
 new=load_profile(ROOT/f'data/{VERSION}/windows-x64.json')
 aliases=dict(re.findall(r'if \(symbol == "([^"]+)"\) symbol = "([^"]+)";', (ROOT/'exsample/CheckSig/src/check_sig.cpp').read_text(encoding='utf-8')))
 inverse={v:k for k,v in aliases.items()}
 legacy={'GameCore':'native_keyboard','Actor':'native_actors','ItemStack':'native_items','LocalPlayer':'native_player_identity','BlockSource':'native_world','StateVectorComponent':'native_motion','Camera':'native_camera','PacketSender':'native_packets','CommandRegistry':'native_client_commands','GuiData':'native_gui','LevelRenderer':'native_world_render','ClientInstance':'native_world_events'}
 groups=[]; requirements=[]
 def group(id,label,roots=(),symbols=()):
  groups.append({'id':id,'label':label})
  for name in symbols:
   requirements.append({'id':'symbol:'+name,'group':id,'kind':'function','symbol':name,'path':'/symbols/'+name,'next_action':'Hookで実際の呼び出し・引数・戻り値を観測。入口一致だけで型や寿命を確定しない。'})
  def walk(value,path):
   if isinstance(value,dict):
    if 'symbol' in value and value['symbol'] in old['symbols']:
     requirements.append({'id':path,'group':id,'kind':'function','path':path,'symbol':value['symbol'],'next_action':'共通識別名からsymbolsの版別定義を解決し、入口・ABI・実呼出しを確認。'});return
    if 'rva' in value and 'entry_check' in value:
     parts=path.split('/'); old_name=legacy.get(parts[2],parts[2])+'.'+'.'.join(parts[3:])
     symbol=inverse.get(old_name)
     req={'id':path,'group':id,'kind':'function','path':path,'next_action':'候補をprobeして周辺・参照・関数境界を確認。対応Observerがあれば観測し、レイアウトへ関数を結び付ける。'}
     if symbol:req['symbol']=symbol
     requirements.append(req); return
    for key,child in value.items():
     if key.endswith('_abi') or key.startswith('observed_'):continue
     if key in {'schema','pointer_size','status','runtime_verified','abi_verified','evidence','source','revision','note','limitations','observed_vanilla_modes','terrain_visibility_status','legacy_game_type_fields_unused','abi'}:continue
     walk(child,path+'/'+key)
   elif isinstance(value,list) and any(isinstance(v,dict) for v in value):
    for i,child in enumerate(value):walk(child,path+'/'+str(i))
   elif isinstance(value,(str,int,float,bool,list)):
    requirements.append({'id':path,'group':id,'kind':'layout','path':path,'reference_only':{'version':OLD,'value':value},'next_action':'現在のオブジェクトの型・オフセット・値・寿命を検証。旧版の値をそのまま新定義へ転記しない。'})
  for root in roots:
   value=old
   try:
    for part in root.strip('/').split('/'):value=value[part]
   except KeyError:continue
   walk(value,root)
 group('input','入力Hook',symbols=['Keyboard::feed','MouseDevice::feed'])
 group('gameplay','ClientInstance・クライアントtick',symbols=['ClientInstance::grabCursor','ClientInstance::releaseCursor','MultiPlayerLevel::_subTick'])
 group('fov','FOV',symbols=['LevelRendererPlayer::getFov'])
 group('keyboard','GameCoreのキー配送',['/layouts/GameCore'],['GameCore::keyboardEvent'])
 group('actors','Actor・EnTT・位置・AABB',['/layouts/Actor'],['Actor::getNameTag','Level::getRuntimeActorList'])
 group('items','ItemStack・装備・インベントリ',['/layouts/ItemStack'])
 group('identity','LocalPlayer・プレイヤー一覧',['/layouts/LocalPlayer'])
 group('motion','StateVectorComponent・移動状態',['/layouts/StateVectorComponent'])
 group('camera','Camera・描画起点・ゲームモード',['/layouts/Camera'])
 group('world','BlockSource・Block・BlockType・NBT',['/layouts/BlockSource/get_region','/layouts/BlockSource/get_block','/layouts/BlockSource/candidate_layouts','/layouts/BlockSource/height_limits'])
 group('placement','GameModeのブロック設置',['/layouts/BlockSource/select_hotbar','/layouts/BlockSource/get_hit_result','/layouts/BlockSource/build_block','/layouts/BlockSource/base_placement_state','/layouts/BlockSource/game_mode_vtables'])
 group('subchunks','LevelChunk・SubChunk走査',['/layouts/BlockSource/subchunk_scan'])
 group('request_binding','CommandRequestPacketの送信バインド',['/layouts/PacketSender/command_request'])
 group('sender','PacketSender',['/layouts/PacketSender/sender'])
 group('packet_interface','Packetの共通仮想インターフェース',['/layouts/Packet'])
 group('movement_packets','共通の送信バイト列Hook（Packet ID / 通信データ読み書き）',['/layouts/PacketWire/batch_append'])
 group('title_packets','共通Packet::readとID88のタイトル読取')
 group('local_commands','CommandRegistryのローカル整数コマンド',['/layouts/CommandRegistry'])
 group('gui','GuiDataのメッセージ表示',['/layouts/GuiData'])
 group('render','LevelRenderer・Tessellator・マテリアル',['/layouts/LevelRenderer'])
 group('world_events','ClientInstanceのワールド切替',['/layouts/ClientInstance'],['ClientInstance::onPlayerLoaded','ClientInstance::onLevelExit','ClientInstance::onDimensionChangedEvent','ClientInstance::onCancelJoinGame'])
 group('command_packets','CommandRequestPacket・CommandOutputPacket')
 def append_new(value,path,group_id):
  if isinstance(value,list):
   for i,child in enumerate(value):append_new(child,path+'/'+str(i),group_id)
   return
  if not isinstance(value,dict):
   if path.split('/')[-1].endswith(('_offset','_rva','_slot')):
    requirements.append({'id':path,'group':group_id,'kind':'layout','path':path,'next_action':'現在のオブジェクトやテーブルに対して値・型・寿命を確認。'})
   return
  if 'symbol' in value and value['symbol'] in new['symbols']:
   requirements.append({'id':path,'group':group_id,'kind':'function','path':path,'symbol':value['symbol'],'next_action':'symbolsの共通識別名で関数を解決。版別RVA/ABIと実機観測を確認。'});return
  if 'rva' in value and 'entry_check' in value:
   requirements.append({'id':path,'group':group_id,'kind':'function','path':path,**({'symbol':'CommandOutputPacket::read'} if path.endswith('/output_read') else {}),'next_action':'入口照合と呼び出し観測。Fake.logで登録・Origin・要求IDと応答の対応・timeoutも確認。'});return
  for k,v in value.items():append_new(v,path+'/'+k,group_id)
 append_new(new['layouts']['PacketRead'],'/layouts/PacketRead','title_packets')
 group('request_sender','MinecraftCommandsからのCommandRequest送信経路')
 append_new(new['layouts']['PacketSender']['sender'],'/layouts/PacketSender/sender','request_sender')
 append_new(new['layouts']['CommandRequestPacket'],'/layouts/CommandRequestPacket','command_packets')
 group('freecam_boundaries','Freecamの退出・ディメンション境界',symbols=['ClientInstance::onLevelExit','ClientInstance::onDimensionChangedEvent'])
 group('freecam_camera','FreecamのLocalPlayer・ダメージ検知')
 for key in ['local_player','hurt_time_component_hash','hurt_time_value_offset','hurt_time_value_size']:
  append_new(new['layouts']['Camera'][key],'/layouts/Camera/'+key,'freecam_camera')
  if key in ['hurt_time_component_hash','hurt_time_value_size']:
   path='/layouts/Camera/'+key;requirements.append({'id':path,'group':'freecam_camera','kind':'layout','path':path,'next_action':'Freecamの読取専用snapshotとダメージ値を確認。'})
 group('message_commands','CommandRegistryのメッセージコマンド')
 append_new(new['layouts']['CommandRegistry']['message_forwarding'],'/layouts/CommandRegistry/message_forwarding','message_commands')
 requirements.append({'id':'/layouts/BlockSource/subchunk_scan/enabled','group':'subchunks','kind':'gate','path':'/layouts/BlockSource/subchunk_scan/enabled','expected':True,'next_action':'BlockStorage getterの実体・ABI・値を確認してから走査を有効化。現時点ではenabled=false。'})
 required={
  'AutoSprint':['keyboard','gameplay'],
  'Zoom':['input','fov','gameplay'],
  'TabList':['input','fov','gameplay'],
  'HitBox':['actors','items','render','gameplay','world_events'],
  'Freecam':['input','gameplay','actors','motion','freecam_camera','request_sender','movement_packets','freecam_boundaries'],
  'SpeedBuilder':['input','gameplay','actors','items','world','placement','title_packets','world_events'],
  'Top':['gameplay','actors','items','world','motion','gui','sender','request_binding','local_commands','world_events'],
  'Fake':['gameplay','request_sender','command_packets','message_commands'],
  'ScanClient':[], 'CheckSig':[]}
 checks={
  'AutoSprint':['Wキーで走る・離すと解除・画面切替後にキーが残らない'],
  'Zoom':['押下でFOV変更・離すと復帰・メニューでは無効'],
  'TabList':['一覧の名前・UUID・Actor対応・装備表示・退出後の消去'],
  'HitBox':['対象プレイヤー判別・AABB位置・移動追従・ワールド切替で旧ポインタを使わない'],
  'Freecam':['カメラだけ移動・送信するプレイヤー位置を維持・無効化時に復帰・再接続後に復帰'],
  'SpeedBuilder':['タイトル検知・記録・設置・インベントリ復帰・キャンセルとワールド切替'],
  'Top':['コマンド候補・整数引数・安全な移動先・位置/AABB/速度・落下状態と高さ・結果表示'],
  'Fake':['/fake候補とメッセージ・Origin省略5/指定0..255・requestId/UUIDと応答・timeout・接続切替'],
  'ScanClient':['EXE識別・read-onlyスキャン・出力ファイルの完結'],
  'CheckSig':['JSON自動読込・入口照合・観測Hook・値読取・入力別結果と残作業の更新']}
 optional={'Freecam':['render','camera','world_events'],'HitBox':['identity'],'TabList':['actors','items','identity'],'ScanClient':[]}
 mods=[{'name':name,'required_groups':deps,'optional_groups':optional.get(name,[]),'manual_checks':checks[name]} for name,deps in required.items()]
 return {'schema':1,'target':{'version':VERSION,'sha256':new['target']['sha256'],'platform':'windows','arch':'x64'},'reference_version':OLD,'implementation_work':[
 {'id':'integer-command-abi','status':'needs-review-after-observation','mods':['Top'],'action':'新ビルドのCommand/CommandParameterData/Overload/整数parse ruleとnative factoryに合わせてローカルコマンドアダプターを対応させる。旧make_parameterがインライン化されている場合は関数検索だけでは解決しない。'},
 {'id':'placement-abi','status':'needs-review-after-observation','mods':['SpeedBuilder'],'action':'GameMode/HitResult/設置状態の新レイアウトと呼出規約を対応させ、設置・取消・インベントリ復帰を確認する。'},
 {'id':'world-render-abi','status':'candidate-implemented-runtime-check-required','mods':['Freecam','HitBox'],'action':'新EXEのLevelRenderer/Tessellator/Material関数と配置を対応。60-world-renderで描画引数と原点・Tessellator・shaderを観測し、枠・名前・状態復元・ワールド切替を実機確認。'},
 {'id':'freecam-layout-assertions','status':'core-updated-runtime-check-required','mods':['Freecam'],'action':'共通pre-batch PacketHookと通信定義のみで、sender帰属・ID19/144・位置固定・InputData配列・条件付きペイロード保持・解除復帰を実機確認。描画追加処理は別途確認。'},
 {'id':'world-event-abi','status':'needs-review-after-observation','mods':['Top','Freecam','SpeedBuilder','HitBox'],'action':'ワールドイベントの真の入口・各引数を確定し、シーン切替時の破棄と再取得を対応させる。'},
 {'id':'additional-observers','status':'needs-review-after-observation','mods':['CheckSig'],'action':'未対応ABIの候補はまずprobeと静的解析で型を確定し、安全な専用Observerまたは呼出元からの観測を追加する。'}],'scope':'source-declared profile dependencies plus exact-build runtime follow-up; not a certificate of Mod compatibility','sources':['src/profile_manager.cpp','include/bedrock/detail/native_world_access.hpp','include/bedrock/native/client_commands.hpp','include/bedrock/native/world_render.hpp','exsample/*/src'], 'groups':groups,'mods':mods,'requirements':list({row['id']:row for row in requirements}.values())}

def make_bundle(out):
 out.mkdir(parents=True,exist_ok=True)
 profile=load_profile(ROOT/f'data/{VERSION}/windows-x64.json')
 catalog=json.loads((ROOT/f'exsample/CheckSig/investigation/requirements-{VERSION}.json').read_text(encoding='utf-8'))
 output=profile['layouts']['CommandRequestPacket']['output_read'].copy()
 output.update(signature='',cross_version_signature_available=False)
 profile['symbols']['CommandOutputPacket::read']=output
 def job(name,actions,watches=None):
  doc={'schema':1,'kind':'checksig-job','arch':'x64','minimum_checksig_features':3,'profile':profile,'requirements':catalog,'actions':actions}
  if watches:doc['watches']=watches
  path=out/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(doc,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');return path
 audit=job(f'commands/{VERSION}-all-mods.json',[{'op':'audit'}])
 for name,symbols in {
  '10-input':['Keyboard::feed','MouseDevice::feed','GameCore::keyboardEvent','ClientInstance::grabCursor','ClientInstance::releaseCursor','MultiPlayerLevel::_subTick','LevelRendererPlayer::getFov'],
  '20-actors-world':['ClientInstance::getLocalPlayer','Level::getRuntimeActorList','Actor::getNameTag','ClientInstance::getRegion','BlockSource::getBlock','BlockSource::getChunk'],
  '30-camera-gui':['sub_6F313F0','sub_5376690','sub_5AFB520','sub_56887B0','Player::setPlayerGameType','GuiData::displayClientMessage','CommandRegistry::registerCommand','CommandRegistry::findCommand'],
  '40-world-events':['ClientInstance::onPlayerLoaded','ClientInstance::onLevelExit','ClientInstance::onDimensionChangedEvent','ClientInstance::onCancelJoinGame'],
  '50-command-output':['CommandOutputPacket::read'],
  '60-world-render':['native_world_render.functions.render_level']
 }.items():
  watches=[]
  def watch(label,symbol,field,path,type):watches.append({'label':label,'symbol':symbol,'field':field,'offsets':path,'type':type})
  if name=='10-input':
   for slot in [32,33,34]:watch('ClientInstance vtable slot '+str(slot),'ClientInstance::grabCursor','self',[0,slot*8],'ptr')
   watch('Rejected ClientInstance+0x1E0 slot2 (RET, not sender)','ClientInstance::grabCursor','self',[0x1e0,0,16],'ptr')
   watch('ClientInstance+0x1E0 rejected sender object','ClientInstance::grabCursor','self',[0x1e0],'ptr')
   watch('CommandRegistry candidate','ClientInstance::grabCursor','self',[0x1d0,0xc8,0x10],'ptr')
   watch('MinecraftCommands client provider','ClientInstance::grabCursor','self',[0x1d0,0xc8,8],'ptr')
  if name=='20-actors-world':
   watch('BlockSource minimum Y candidate','ClientInstance::getRegion','return_block_source',[0x3a],'i16')
   watch('BlockSource maximum Y candidate','ClientInstance::getRegion','return_block_source',[0x38],'i16')
   watch('BlockSource getBlock vtable slot2','ClientInstance::getRegion','return_block_source',[0,16],'ptr')
   watch('BlockSource getChunk vtable slot41','ClientInstance::getRegion','return_block_source',[0,328],'ptr')
   c=profile['layouts']['Actor']['actor_context']
   # Context offsets come from the new profile, never from the old reference.
   watch('Actor header candidate','ClientInstance::getLocalPlayer','return_actor',[0],'bytes:64')
  if name=='60-world-render':
   symbol='native_world_render.functions.render_level';offsets=profile['layouts']['LevelRenderer']['offsets']
   watch('LevelRendererPlayer','native_world_render.functions.render_level','renderer',[offsets['level_renderer_player']],'ptr')
   for axis in range(3):watch('render origin '+str(axis),symbol,'renderer',[offsets['level_renderer_player'],offsets['render_origin']+axis*4],'f32')
   watch('ScreenContext Tessellator',symbol,'screen',[offsets['screen_tessellator']],'ptr')
   watch('ScreenContext shader color',symbol,'screen',[offsets['screen_shader_color']],'ptr')
   watch('Tessellator building',symbol,'screen',[offsets['screen_tessellator'],offsets['tess_building']],'u8')
   watch('Tessellator special mode',symbol,'screen',[offsets['screen_tessellator'],offsets['tess_special_mode']],'u8')
  job('optional/'+name+'.json',[{'op':'observe','symbols':symbols},{'op':'audit'}],watches)
 source=out/'reference'/f'windows-x64-{OLD}.json';source.parent.mkdir(parents=True,exist_ok=True);source.write_text(json.dumps(load_profile(ROOT/f'data/{OLD}/windows-x64.json'),ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
 port=job('optional/90-port-old.json',[{'op':'port'},{'op':'audit'}]);doc=json.loads(port.read_text(encoding='utf-8'));doc['source']=json.loads(source.read_text(encoding='utf-8'));port.write_text(json.dumps(doc,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
 shutil.copy2(ROOT/'docs/checksig-126605.md',out/'README_ja.md')
 return audit

if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--catalog',action='store_true');p.add_argument('--bundle',type=Path);args=p.parse_args()
 if args.catalog:
  path=ROOT/f'exsample/CheckSig/investigation/requirements-{VERSION}.json';path.write_text(json.dumps(generate_catalog(),ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
 if args.bundle:make_bundle(args.bundle)
