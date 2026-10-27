# Camera・GameType・Brightness API

Freecam専用の新しいAPIは追加しません。`CameraRig`（コピー値の計算）と
`NativeCamera`（ローカルPlayerプロキシの操作）を組み合わせます。
`FreeCameraRig`は既存ソースのための互換名です。

## カメラを動かす

```cpp
bedrock::CameraRig rig;
rig.begin(saved.position, saved.bounds, saved.runtimeId, saved.worldEpoch);
auto frame = rig.advance(bedrock::Yaw{90.f}, {1.f, 0.f, 0.f}, {10.f, 5.f},
    std::chrono::duration<float>{0.05f}, saved.runtimeId, saved.worldEpoch);
rig.look(bedrock::Yaw{90.f}, bedrock::Pitch{-15.f});
auto displayPosition = rig.sample(0.5f);
```

速度はblock/秒、時間は秒の`chrono::duration<float>`です。入力の各軸は-1..1。
時間0..0.25秒、有限かつ非負の速度を受理し、不正入力は`nullopt`で拒否します。
旧`update(float,...)`は互換用として1回あたりの移動量を維持します。
`moveTo(Vec3)`、`frame()`、`rotation()`、`anchor()`、`active()`、`reset()`もあります。
RuntimeIDまたはworld epochが変わると`advance`は状態を失効させます。

## NativeCamera

Windows x64 / MSVC Release、`bedrock::world`（EnTT）を使用します。
`ProfileManager::prepare`のアダプター内で`camera.load(profile,error)`を呼びます。
必要FeatureはCameraとMotionです。

```cpp
auto scope = camera.tick(clientInstance, bedrock::WorldEpoch{epoch});
if (!scope) return;
auto snapshot = camera.readPlayerProxy();
if (snapshot && frame)
    camera.applyPlayerProxy(*frame, snapshot->runtimeId, bedrock::WorldEpoch{epoch});
camera.setRotation(bedrock::Yaw{90.f}, bedrock::Pitch{0.f});
// 終了時に保存したsnapshotを戻す。異なるworld/runtimeは拒否。
// camera.restorePlayerProxy(saved);
```

操作はscopeのあるLevelTick上に限定されます。現在位置・前回位置・AABB・速度・落下距離の
更新はSDKが行います。AABBは現在の形状から平行移動し、引数の体サイズを信用しません。
結果は`CameraChange`。WorldChangeでは`camera.reset()`と`rig.reset()`を呼びます。
これはゲームが追尾するローカルPlayerのプロキシです。ネットワーク上の元位置保持、
インタラクション、入力制御、RenderDragonの地形表示方針は別の責務です。
サーバーへ送る座標を自動で変更したり、入力を奪ったりしません。

## GameType

カメラや描画とは独立した`NativeGameType`を使います。
`load(profile,error)`に必要なFeatureはGameTypeです。

```cpp
auto scope = types.tick(clientInstance, bedrock::WorldEpoch{epoch});
if (!scope) return;
std::optional<bedrock::GameType> current = types.gameType();
if (types.canChangeGameType()) {
    bedrock::GameTypeChange result = types.setGameType(bedrock::GameType::Creative);
}
```

Survival=0 / Creative=1 / Adventure=2 / Default=5 / Spectator=6。
無効なenum値、別スレッド、古いworld/runtime、欠落したコンポーネントは拒否します。
WorldChangeで`types.reset()`を呼びます。
`GameTypeAccess`を実装すれば別アダプターへ差し替えられます。

1.26.52.3の変更関数は静的解析に基づく**candidate**です。
PlayerのGameModeを差し替え、GameTypeとLayeredAbilitiesを本体関数で更新する経路を使用します。
RVA/entry guard/ABIはプロファイルへ集約し、整数コンポーネントだけを直接変更する代替はありません。
未定義の版では`canChangeGameType()==false`、変更要求は`Unsupported`です。
変更はローカルゲーム状態への要求であり、マルチプレイの権限やサーバー側のモードを付与しません。
本体のsetterは他のローカル状態も変更するため、単純な一時描画オプションとして使用しないでください。

## ガンマ・Full bright

```cpp
bedrock::NativeBrightness brightness;
// prepareの中: brightness.load(profile,error)
// 全アダプター検証後: brightness.install(error)
brightness.setGamma(bedrock::Gamma{2.f});
brightness.fullBright(); // Gamma{25.f}
brightness.reset();     // 自分の要求を解除。元のgetterの値に戻る
```

Gammaは有限値0..25。`Feature::Brightness`が必要で、EnTTは不要です。
Optionsの保存値を書き換えず、ガンマgetterの戻り値だけを変更します。
同じSDKモジュール内の複数オーナーは最大値を合成し、他オーナーの要求は解除しません。
HookはDLLの寿命に合わせてpinされ、要求がない間は元のgetterの値を返します。
別DLLのHook競合は`install`で失敗を返し、強制的に上書きしません。

## 確認範囲

- GameType setter: EXE全体のREA/Ghidra解析は起動期限に達したため、`.pdata`で切り出した関数を原VAにマッピングしてREA/Ghidraで解析。
- 主関数`0x1E1570..0x1E1B0A`、補助関数`0x2096F0`。
- 主関数はRCX Player / EDX int、helperはRCX EntityContext / EDX mode / R8D default。
- GameModeフィールド`+0xAA0`は既存native_world定義と一致。helperはGameTypeComponentへ書き込み、LayeredAbilitiesを更新。
- REA: `ev_3f502740423ae916ab45e21e355d2e6b68a16a2a7111cf3002659742ffec8d3c`、`ev_4f7bcdac8b9a59cc7a783be7442ad8ae63013fbc568505b3138e976a12e03049`。
- raw importのgcc疑似コード型はWindows ABIの根拠にせず、PEのレジスター操作を照合。実ゲームの実行は未確認。
