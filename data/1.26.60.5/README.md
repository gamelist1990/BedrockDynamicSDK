# 1.26.60.5 · Windows x64

更新日：2026-11-14。対象EXE SHA-256：
`988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a`

2026-11-13〜14に自動テスト環境（BedrockOnLinux＋DLL注入）と外部サーバー（The Hive）・
ローカルワールドで全Modの動作を確認し、本バージョンを正式リリースとしました。

## 動作状況

- [x] **Freecam**：正常動作のユーザー確認済み。カメラ更新・移動状態・送信PacketHook・senderの使用範囲をverifiedへ反映。terrain culling／view factory の定義も解決し ready を確認。2026-11-14に The Hive で wire PacketHook 確立、Mキーでの有効化、パケット位置固定、カメラ飛行、解除時の復元まで実証。
- [x] **Zoom**：正常動作のユーザー確認済み。入力・FOVの使用経路をverifiedへ反映。自動検証でもワールド内動作を確認。
- [x] **Fake**：正常動作のユーザー確認済み。`/fake`登録・本文取得・`/me`要求・応答処理を反映。ワールド内でCommandRegistry登録を確認。
- [x] **TabList**：パケット別デコーダーへのHookと専用解析ワーカーを実装。2026-11-13に The Hive ワールド内で PlayerList の受信・解析を実証。
- [x] **HitBox**：2026-11-13にワールド内で対象取得（native capture）・NameTag描画・実名表示を確認。
- [x] **SpeedBuilder**：タイトル受信経路を共通PacketAPIへ移行。設置ABI（hotbar選択・ヒット結果・buildBlock・配置状態）を解決。2026-11-13にローカルワールドで native builder 起動・タイトル記録・範囲キャプチャ・ホットバー読取りを確認。再構築フェーズは2026-11-14に自前BDSで実証済み（下記）。
- [x] **Top**：GuiData・CommandRegistry の定義を解決。2026-11-13にローカルワールドで `/top` の登録・パース・実行、安全ゲートのゲーム内表示まで実証。2026-11-14に自前BDS（1.26.60.6、サーバー権威移動）で再実行し、頭上の石スラブへの移動書込みと成功表示（足元 Y=86）までは動作するが、位置はサーバー側の移動検証で即座に巻き戻され定着しないことを確認。クライアント側状態書換え方式の制約であり、ローカルワールド／クライアント権威のサーバーでは成立する。
- [x] **AutoSprint**：2026-11-13にワールド内で動作を確認（HIDController経路）。
- [x] **CheckSig**：2026-11-13の定義解決作業全体で使用し、ジョブ実行・auto-dev生成・portの動作を確認。
- [x] **GlobalResourcePack**：正常動作のユーザー確認済み（v21）。自動検証でプロファイル一致・Hook導入・ワールド内有効を確認。

## 関数の確認範囲

`verified`は各定義の`verification_scope`と合わせて読む。入口特定とMod動作確認は別の根拠。
数値のオフセットが読めるだけでは、オブジェクト全体のABIや寿命は確認済みにしない。

| 範囲 | 件数 | 状態 |
| --- | ---: | --- |
| 実行・Mod動作の確認あり | 13 | [x] runtime_verified |
| 入口ガードの確認まで | 27 | [x] 入口特定 / [ ] 動作全体 |
| 関数確認待ち | 1 | [ ] 未検証 |

### 実行・Mod動作の確認あり

- [x] `ClientInstance::getLocalPlayer`
- [x] `ClientInstance::grabCursor`
- [x] `ClientInstance::releaseCursor`
- [x] `CommandRegistry::findCommand`
- [x] `CommandRegistry::registerCommand`
- [x] `GuiData::displayClientMessage`：Topの安全ゲート表示で実行を確認（2026-11-13）。
- [x] `Keyboard::feed`
- [x] `LevelRendererPlayer::getFov`
- [x] `MouseDevice::feed`
- [x] `MultiPlayerLevel::_subTick`
- [x] `sub_56887B0`
- [x] `sub_5AFB520`
- [x] `sub_6F313F0`

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
- [x] `sub_5AD5F90`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_65A8500`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_65BD080`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_76A710`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_8AB690`：入口確認済み。実行・ABIの確認範囲は定義内を参照。
- [x] `sub_B94CA0`：入口確認済み。実行・ABIの確認範囲は定義内を参照。

### 解決済み（2026-11-13〜14）

- [x] `layouts.GuiData`：getter走査と実メモリ指紋で `client_gui_offset=0x668`・get_gui・display_message を確定し、60.5データへ追加。
- [x] `layouts.CommandRegistry`：登録・検索・任意引数・factory・integer記述子を確定。`make_parameter` は単体関数が存在しないためSDK側でパラメータをインライン構築する方式へ変更。
- [x] `Camera.visual.terrain_culling`：set_culler・culler factory・spectator vtable・update を特定し、オフセット（0x9A0／0x9B0）を確定。ready を実機確認。
- [x] `Camera.visual.create_view`：createViewRenderObject の入口を特定し、view factory ready と生成を確認。`inside_cube_render`（draw_inside_cubes）は根拠が無いため定義を省略し、このサブ機能は無効のまま（推定で埋めない）。
- [x] `BlockSource`の設置ABI：select_hotbar・get_hit_result・buildBlock・配置状態スロットを特定。
- [x] `PacketRead` revision2：TabListのワールド内受信実証により、新デコーダー経路での受信を確認。

### 未検証・残作業

- [ ] `Level::getPlayerList`：候補のまま。TabListは通信PlayerListを使うため、このコンテナの推測値に依存しない。
- [ ] `LocalPlayer`のネイティブPlayerListコンテナ・skin配置：`Level+0x4E8`は版別定義済み。同上、通信経路で代替済み。
- [x] SpeedBuilder再構築フェーズ：2026-11-14に自前BDS（1.26.60.6）で実証。player_game_mode_offset は52.3引継ぎ候補 0xAA0 が誤りで、実機スキャンで 0xAC8 と特定（GameMode vtable 0xF1A8F30＋owner逆参照で一意）。capture 9261セル→土ブロック手掘り→diff確定（removed=1）→ドロップ拾得→設置計画→placement native accepted=1、復元を目視確認。

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
