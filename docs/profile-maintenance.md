# Windows x64定義とSDKの更新

編集元は`data/<version>/windows-x64/classes/*.json`です。`manifest.json`でパスとSHAを管理します。詳細は[構成ガイド](sdk-structure.md)を参照してください。
CheckSigへ単一ファイルで渡す場合だけ`profile.json`を明示的にexportします。旧windows-x64.jsonは配布・自動生成しません。

## 責務

| 場所 | 管理するもの |
| --- | --- |
| `data/<version>/windows-x64/classes/*.json` | EXE識別、RVA、入口ガード、シグネチャ、ABI、vtable slot、構造体とフィールド位置、候補・観測状態 |
| SDK `include/bedrock/native/` / `detail/` | 関数呼出し、ネイティブABI、メモリ読取・書込、寿命・スレッド・範囲検査、コピーへの変換 |
| Mod | SDKを呼ぶ条件、表示、設定、ゲーム機能の組合せ。アドレス探索・ネイティブ構造体・版別数値を追加しない |

`0x4E8`をModへ書き込まず、PlayerList APIの定義へ置きます。SDKはこの値を新しい版の既定値としても使いません。
通信フォーマットはwire protocol定義、C++メモリの配置はx64 native定義です。両者を同じオフセットとして扱いません。

## 識別名と参照

```json
{
  "symbols": {
    "Level::getPlayerList": {
      "rva": "0x232F0B0",
      "entry_check": "48 8B 81 E8 04 00 00 C3",
      "signature": "",
      "cross_version_signature_available": false,
      "status": "candidate",
      "abi": "PlayerList* __fastcall(Level*)"
    }
  },
  "layouts": {
    "LocalPlayer": {
      "get_list": {"symbol": "Level::getPlayerList"},
      "level_player_list_offset": 1256
    }
  }
}
```

これは説明用の抜粋です。実ファイルにはtarget、schema、その他の必須項目も必要です。
`Level::getPlayerList`や`ClientInstance::getLocalPlayer`は版をまたいで維持するSDKの識別名です。EXE内のデバッグ名が取得できたことは意味しません。
同じ意味の関数は同じ識別名を維持し、各版のRVAと根拠を更新します。意味やABIが違う場合は、同一名だからという理由で呼びません。

関数の実定義は`symbols`に一つ置きます。layoutの`{"symbol":"..."}`はSDKの`parseNativeProfile`が読み込み時に展開します。
ファイルに`native_*`の別名や関数本体の複製を増やさず、SDK内で互換ビューを作ります。参照先の欠落はエラーです。

## プロファイルを選ぶ順序

`ProfileManager::prepare`の通常経路は次の通りです。

1. 保持中のsnapshotを、要求機能・アダプターで再確認する。
2. 通常の取得処理が選んだプロファイルを、ロード中EXEのSHA・PE geometryと照合する。
3. 選んだ定義に必要な機能がなければ、同一EXE用の内蔵定義を試す。
4. それでも不足する場合に、GitHubから同一EXE用定義を更新する。
5. 取得・検証できなければ機能を停止し、足りない定義をログへ出す。

`ForceRemote`はGitHubを先に試します。キャッシュが新しいという理由だけで、SDKが必要とするlayout revisionを省略しません。
起動中に既存Hookを別アドレスへ付け替える仕組みではありません。DLLと定義の差し替え後はゲームを再起動します。

`NativePlayerList::load(selected, embedded)`はPlayerList専用の補完です。選択定義に必要なrevisionがなければ、SHA・text SHA・image/text geometryが一致する内蔵定義だけを試します。独自のネットワーク探索や別バージョンへのfallbackは行いません。
古いModの独自起動処理はまだ`ProfileManager`へ統一されていません。下記の残存箇所を順次SDK APIへ移します。

## 関数を解決・調査する順序

### 実行時

SDKアダプターは、選択済みの同一EXE定義の識別名 → RVA → `.text`範囲 → 入口ガード → 必要なABI/layout検査の順に確認します。
共有Hookが所有する入口は、対応するbrokerが同一point/targetを所有している場合だけパッチ済み入口として扱います。
シグネチャが一致しなかったから旧版のRVAやオフセットで呼ぶ、という自動復帰は行いません。
一般のResolverは候補探索もできますが、nativeアダプターの呼出し許可とは別です。

### 新しいEXEの調査

1. SHA、ファイル版、image base、`.text`範囲を記録する。
2. 旧版の同じ識別名と、その関数の役割・ABI・呼出元の根拠を読む。
3. 根拠のある入口・本体anchorを使い、候補の一意性を調べる。
4. 一意でなければ文字列・RTTI・呼出元/先・vtableの隣接関係から候補を絞る。slotが不変という仮定だけで決めない。
5. REA/Ghidraで命令、引数、戻り値、hidden return、オブジェクト型、フィールド、関数境界を確認する。
6. 新版の`symbols`と`layouts`をcandidateとして更新する。新しい呼出規約や共通ABIならSDKも更新する。
7. CheckSigで入口・Hook・引数/戻り値/フィールドを観測する。最後にModの見た目・解除・ワールド切替も確認する。

入口一致だけでオブジェクト配置や機能全体をverifiedにしません。短いleaf getterは`.pdata`を持たない場合があります。その場合はleafの命令と呼出関係を根拠として残し、Hook可能とは別判定にします。
根拠はEXE SHAとともに`Documents/PEXData/REA/`へ保存し、定義の`source`・`evidence`から調査を辿れるようにします。

## PlayerList

| 項目 | 意味と更新方法 |
| --- | --- |
| `symbols.Level::getPlayerList` | mapへのポインタを返す関数。現在はLevelのslot319、getRuntimeActorListの6枠前から相互確認。将来もslot319とは断定しない |
| `get_list` | SDKは検証できたgetterを優先。定義があるのに入口が違う場合は失敗する |
| `level_player_list_offset` | getterを持たない定義に対する明示的な読取経路。1.26.52.3は0x4E0、1.26.60.5は0x4E8。Modには渡さない |
| `entry_size` / `*_offset` | entry内のUniqueID、UUID、名前の位置。getterの入口確認だけでは検証できない |
| `map_abi` | Release MSVC STL、UUID key、8-byte aligned valueというSDKの対応ABI。ゲームが別container/key/alignmentへ変わればSDKアダプターも変更する |
| `skin.*` | entryのskin pointer、image、width/height/data/sizeの位置。全て版別JSON。画像が読めても寿命は未検証 |
| `maximum_*` | 読取件数・名前長の上限。無制限なポインタ走査にしない |

SDKはゲームのmapを読み取り、所有する`PlayerRosterEntry`へコピーします。ゲームのallocatorやhash検索を呼びません。顔画像もSDKが64個のコピー済みピクセルにします。
Modにはmap、entry、skinのポインタやオフセットを返しません。空一覧と取得失敗を区別し、古いワールドの値を持ち越さないでください。

```cpp
bedrock::NativePlayerList roster;
roster.load(selectedProfile, embeddedExactBuildProfile);
// 所有するgame tick内で実行。workerへ渡せるのはrows内のコピーだけ。
if (auto rows = roster.snapshot(level)) {
    for (const auto& player : *rows) {
        // player.name / player.uuid / player.uniqueId
    }
}
```

LocalPlayer取得も`NativeLocalPlayerGetter::load(profile)` / `get(client)`を使います。
関数は`ClientInstance::getLocalPlayer`で識別し、旧vtable slotを使う必要がある場合もSDKが定義から読みます。Modからslotやoffsetを指定しません。

## 自動探索

[CheckSig auto-dev](checksig-auto-dev.md)で関数・vtable・命令の根拠を生成できます。未解決のABIや構造体を旧版の値で埋める機能ではありません。

## 残存する旧実装

今回のPlayerList/skin/LocalPlayer移行で全Modのネイティブ処理が消えたわけではありません。次をSDK境界へ移す必要があります。

| 箇所 | 残っている処理 |
| --- | --- |
| Freecam | culler、view factoryのHook管理・ネイティブ描画の一部。InsideBlockEffect・Actor component・Player bounds追跡はSDKへ移行済み |
| HitBox | EnTT/Actorコピー、描画・名前キャプチャの一部 |
| TabList | equipmentのItemStackコピー・キャッシュとネイティブイベント連携。Actor component / runtime ID / game type / packet rosterはSDKへ移行済み |
| SpeedBuilder | タイトル受信は旧版fallbackも含めTitlePacketsへ移行済み。設置・world読取はSDK detailを使用 |
| その他のexamples | 各native呼出しをSDK APIへ移す際に調査。UI用座標や色、入力キーはnative layoutとは区別する |

これらへ新しい版別数値を足して拡張せず、触る機能単位でSDKアダプターへ移してください。
新しい共通ABIにSDK変更が必要なのは正常です。単なるRVA・同一ABI内のフィールド移動ならx64定義の更新に閉じる設計にします。
