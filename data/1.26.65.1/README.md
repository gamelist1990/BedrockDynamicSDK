# 1.26.65.1 · Windows x64

作成日：2026-11-17。対象EXE SHA-256：
`dacd67e72d7f2d6bc55c36b5189ff91d0d17ccedeee5277ebeab8013b4093b88`
（.text SHA-256：`87a97cd774cd9ac5c51a6f0b70064eb220cbe803b981f5d8de8804ad88f51796`）

1.26.60.5 からのバージョン更新。ユーザーの実機（Microsoft Store版）が
1.26.65.1 へ自動更新され、旧プロファイルが PE geometry mismatch で
弾かれたことが発端。

## 移植方法と検証状態

- シンボル74件すべてを解決。43件は `bedrock-symbols port` のAOB自動移植、
  残り31件は移植済み近傍シンボルのRVAデルタ予測＋entry check照合＋
  候補関数の逆アセンブル比較で特定した。
- **全74関数について、1.26.60.5（verified済み）と1.26.65.1の命令列
  （mnemonic列）が完全一致**し、命令数・フィールドオフセット・vtable
  スロットも同一だった。60.5でverifiedだったものは根拠を明記して
  verifiedへ昇格し、candidateだった2件（Level::getPlayerList、
  ResourcePackManager::applyPendingStack）はcandidateのまま。
- レイアウト内の関数ポインタ（69件）と、手書きvtableテーブル8件、
  BlockSource の vtable候補リスト／GameMode vtableリストは、関数
  マップ（118件、オペランド単位で監査済み）と.rdata上の実参照から
  個別に再配置した。旧値のまま残っていたリスト形式RVA 5件が
  ブロック読み取り失敗の原因だった（下記）。
- プロトコル参照は data/protocols/1.26.60（protocol 2225）を継承。
  1.26.65.1 の BDS へ参加し、CraftingData（3198レシピ）取得と
  ItemStackRequest クラフトが実地で動作したため、この参照で実用上
  問題ないことを確認した。
- `NativePacketSender` の MinecraftCommands 経路は対象EXEハッシュを
  コード側で固定確認しているため、65.1 のハッシュを許可リストへ追加
  した（include/bedrock/native/packet_sender.hpp）。経路のオフセット
  チェーンは65.1でも不変で、実地で解決できた。
- pytest 15件パス（2026-11-17）。オフライン自己照合：entry_check
  143組すべて一致、全シンボルの署名が新EXEのRVA位置で一致。

## 動作状況（2026-11-17、BedrockOnLinux＋自前BDS 1.26.65.1）

- [x] 全Modビルド（build-linux.sh、LATEST=1.26.65.1）
- [x] 注入・プロファイル解決（埋め込みプロファイルで exact match）
- [x] AutoMine：/mine 登録・ローカル実行、ブロック走査／識別
      （grass_block を正しく識別）、A* 経路探索、採掘・回収・
      目標達成（grass_block ×3 完走）
- [x] AutoMine 遠征：木材20個の自動収集ステージ完走
- [x] AutoCraft：板材・作業台・木のツルハシの実クラフト
      （ItemStackRequest）、作業台の設置／近傍台の再利用、インベントリ
      スナップショットの個数一致
- [ ] 他Mod（CheckSig / Top / Freecam / HitBox / SpeedBuilder /
      TabList / Zoom / AutoSprint / Fake / GlobalResourcePack）の
      smoke test は未実施。AutoMine が通した経路のシンボル／レイアウト
      関数エントリのみ runtime_verified を true に戻し、残りは
      実地確認後に昇格する。

## ハマりどころ（次回更新時の注意）

- プロファイルキャッシュ（%LOCALAPPDATA%\PEXData\BedrockDynamicSDK\
  profile-cache\）は埋め込みプロファイルより優先される。データ修正後
  はキャッシュを消さないと旧プロファイルで解決され続け、修正が
  効いていないように見える。
- レイアウト内の「リスト形式のRVA」（vtable候補リスト等）はスカラー
  値と違って自動再配置の対象から漏れやすい。新旧プロファイルの
  全RVA値を棚卸しし、旧.rdata/.data を指したままの値が残っていない
  か確認すること（今回のブロック読み取り失敗の真因）。
- BOL のステージング済みEXEは正規EXEとヘッダ数バイトが異なるが
  .text ハッシュは同一で、ランタイムの同一性判定（file hash OR
  text hash＋geometry）はこれで通る。
