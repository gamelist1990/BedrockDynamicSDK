# 1.26.60.5 · Windows x64

更新日：2026-10-29。対象EXE SHA-256：
`988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a`

## 動作状況

- [x] **Freecam**：正常動作のユーザー確認済み。カメラ更新・移動状態・送信PacketHook・senderの使用範囲をverifiedへ反映。
- [x] **Zoom**：正常動作のユーザー確認済み。入力・FOVの使用経路をverifiedへ反映。
- [x] **Fake**：正常動作のユーザー確認済み。`/fake`登録・本文取得・`/me`要求・応答処理を反映。
- [ ] **TabList**：旧ログは受信0件・行0件。パケット別デコーダーへのHookと専用解析ワーカーを実装。実機で受信・表示を再確認する。
- [ ] **HitBox**：起動と描画Hook到達の記録あり。実際の対象取得・box表示は未確認。
- [ ] **SpeedBuilder**：タイトル受信経路を共通PacketApiへ移行。新しい受信経路と設置処理の実機確認が必要。
- [ ] **Top / AutoSprint / CheckSig**：入口特定と調査記録あり。この版の全機能の正常動作確認とは分けて扱う。Topの追加修正は後回し。

## 関数の確認範囲

`verified`は各定義の`verification_scope`と合わせて読む。入口特定とMod動作確認は別の根拠。
数値のオフセットが読めるだけでは、オブジェクト全体のABIや寿命は確認済みにしない。

| 範囲 | 件数 | 状態 |
| --- | ---: | --- |
| 実行・Mod動作の確認あり | 9 | [x] runtime_verified |
| 入口ガードの確認まで | 31 | [x] 入口特定 / [ ] 動作全体 |
| 関数確認待ち | 1 | [ ] 未検証 |

### 実行・Mod動作の確認あり

- [x] `ClientInstance::getLocalPlayer`
- [x] `ClientInstance::grabCursor`
- [x] `ClientInstance::releaseCursor`
- [x] `CommandRegistry::findCommand`
- [x] `CommandRegistry::registerCommand`
- [x] `Keyboard::feed`
- [x] `LevelRendererPlayer::getFov`
- [x] `MouseDevice::feed`
- [x] `MultiPlayerLevel::_subTick`

### 入口特定済み、実行・用途別確認待ち

- [x] `Actor::getNameTag`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `BlockSource::getBlock`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `BlockSource::getChunk`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `ClientInstance::getRegion`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `ClientInstance::onDimensionChangedEvent`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `ClientInstance::onLevelExit`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `ClientInstance::onPlayerLoaded`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `GameCore::handleMouseInput`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `GameCore::keyboardEvent`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `GameMode::buildBlock`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `GuiData::displayClientMessage`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `ItemStackBase::getHoverName`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `Level::getRuntimeActorList`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `MovePlayerPacket::isValid`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `Player::setPlayerGameType`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `PlayerAuthInputPacket::getId`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `PlayerAuthInputPacket::isValid`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `Tessellator::vertex`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_4DE7250`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_5376690`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_537E6E0`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_5417230`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_56887B0`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_5AD5F90`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_5AFB520`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_65A8500`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_65BD080`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_6F313F0`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_76A710`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_8AB690`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_B94CA0`：入口確認済み。実行・ABIの確認範囲は定義内を参照。

### 未検証・残作業

- [ ] `Level::getPlayerList`：候補。
- [ ] `Camera.visual.terrain_culling`：selector・spectator vtable・updateの対応を未確定。Freecam全体の正常動作から推定で埋めない。
- [ ] `Camera.visual.create_view` / `inside_cube_render`：新ビルドの一意な関数入口が未特定。
- [ ] `PacketRead` revision2：ID別vtableでslot15/16を探索し、共通ラッパーの受信0件を修正。静的解析済み、実機での受信確認待ち。
- [ ] `LocalPlayer`のネイティブPlayerListコンテナ・skin配置：`Level+0x4E8`は版別定義済み。TabListは通信PlayerListを使うため、このコンテナの推測値に依存しない。
- [ ] `BlockSource`の設置・SubChunk関連：未解決のABI・ゲートは定義中の理由を確認。

## 定義の場所と更新順序

| 場所 | 役割 |
| --- | --- |
| [windows-x64/manifest.json](windows-x64/manifest.json) | EXE識別、protocol参照、各ファイルのパスとSHA |
| [classes/](windows-x64/classes/) | Minecraftクラスごとの関数・offset・vtable・ABI |
| [analysis/](windows-x64/analysis/) | CheckSig観測、静的解析、ユーザー確認の根拠 |
| [metadata/maintenance.json](windows-x64/metadata/maintenance.json) | 探索・更新方針 |

1. 同じEXEのSHA・PE範囲を確認する。
2. 共通の`Class::function`名を維持し、入口ガード・一意なsignature・vtable相関・呼出元の順で根拠を照合する。
3. 変更は該当クラスJSONとSDKアダプターに入れる。ModにRVA・offsetを追加しない。
4. manifestの各part SHAをrefreshし、`data/manifest.json`を更新する。
5. CheckSigで不足範囲を観測し、GitHubのWindows RunnerでDLLをビルドする。

旧`windows-x64.json`とScanClientは廃止。CheckSigへ単一ファイルで渡す場合は明示的に`profile.json`へexportする。
メモリ範囲調査は[CheckSigのJSONジョブ](../../docs/checksig-jobs.md#メモリ範囲を読む)を使う。
[保守手順](../../docs/profile-maintenance.md) · [パケットAPI](../../docs/packet-events.md)
