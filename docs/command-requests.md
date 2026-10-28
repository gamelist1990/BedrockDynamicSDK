# CommandRequest / CommandOutput API

`<bedrock/command_requests.hpp>` は通信方式に依存しない要求・応答APIです。
Windows x64 MSVC Release向けの `<bedrock/layouts.CommandRequestPacket.hpp>` が
ゲームのPacketSenderとCommandOutputPacketに接続します。
`bedrock::sdk` と `bedrock::hook_runtime` をリンクしてください。

```cpp
bedrock::NativeCommandRequests transport; // retain for process lifetime

// Define local text handlers before load(). Unrecognized commands pass through.
transport.addLocalCommand("hello", [&](std::string_view fullLine) {
    bedrock::CommandRequestOptions options;
    options.origin = static_cast<std::uint8_t>(bedrock::CommandOriginType::AutomationPlayer);
    options.timeout = std::chrono::seconds{10};
    transport.commands().request("/me Hello", options)
        .then([](const bedrock::CommandResponse& response) {
            if (response.status == bedrock::CommandResponseStatus::Received) {
                // response.successCount, outputType, messages[i].id,
                // messages[i].successful, messages[i].parameters
            }
        });
});
```

## 初期化とtick

`ProfileManager::prepare()` に Gameplay / LevelTick の要件と
`CandidatePolicy::AllowCandidates` を指定し、adapter内で
`transport.load(profile, error)` を呼びます。
`GameHooks::installGameplay()` / `installLevelTick()` でイベントを接続し、
Gameplayから観測したClientInstanceとLevelTickのlevelを
`transport.update(client, level)` へ渡します。
設定とイベント購読を終えてからフックを有効にしてください。
起動から停止までの例は [Fake](../exsample/Fake/src/fake.cpp) にあります。

要求の作成・結果の受信はスレッドセーフです。送信と `then` は
`update()` の所有tickスレッド上で実行します。
同じFutureに複数の `then` を登録でき、完了後の登録も次のpollで通知します。
`future.cancel()` は次のpollで取消、`future.result()` は完了結果のコピーを返します。
破棄前にゲームイベントを解除して `transport.stop()` を呼んでください。
停止後はネイティブ送信もtick通知も再開しません。未通知の結果はFutureの `result()` で取得できます。

## 引数と結果

- SDK単体の `CommandRequestOptions.origin` の既定値はPlayer=0。Fake Modが5を指定します。
- `origin` は `uint8_t`。文字列からの変換時には呼び出し側で0〜255を検証してください。
- `playerId` はDevConsole/Test用の追加データです。`version`、`internal` も指定できます。
- ネイティブadapterはversion=0の場合、直近のゲームのCommandRequestから観測したversionを利用します。
  まだ観測していない場合は0を送ります。
- 各要求はOS乱数による独立したUUIDとRequestIdを持ち、応答の両方が一致した場合だけ完了します。
- `Received` はサーバーからの受信。コマンド成功は `succeeded()` または `successCount` で判定します。
- Timeout / Disconnected / Cancelled / InvalidRequest / Unavailable / QueueFullも `then` に通知します。
- 要求は最大128件、ローカル文字列コマンドは最大64件。タイムアウトは1〜300000ms。
- 応答の文字列は所有コピーです。メッセージ64件、各パラメーター32件、文字列4096バイト、合計64KiBまで。
  レイアウト・境界が不正な応答は採用せず、該当要求は応答待ちを継続します。
- `hasDataSet` はPropertyBagの有無のみです。追加データ本体や翻訳処理は提供しません。

ワールドとClientInstanceの変更は次のtickで検出し、保留中の要求を終了します。
ゲームのtickが止まっている間は期限切れ通知も遅延します。
応答しないOriginやサーバー設定でも、送信しただけで成功と判定することはありません。

## 対象ビルドと根拠

現adapterはデータ定義の `layouts.CommandRequestPacket` schema1 / layout_revision1を読み、
1.26.60.5の完全一致EXE、getter/constructor/getId/decodeのガード、vtable、
送信関数とdecode関数の `.pdata` 開始位置を確認します。
新レイアウトは **static candidate / runtime_verified=false** です。
既存43シンボルの入口確認とは別の検証範囲です。

REA/Ghidraで確認した構造:

| 対象 | 内容 |
| --- | --- |
| Request constructor RVA0x41989A0 | command+0x30, Origin+0x50, Version+0x90, Internal+0x94, serialization5+0x98, size0xA0 |
| Command execution RVA0x577A6F0 | スタック上の要求をsenderのslot2に渡して直後に破棄。同期借用による送信 |
| Getter RVA0x8AB680 | ClientInstance+0x1E0を返す8バイトのleaf。隣の0x8AB690は使用しない |
| Output read RVA0x4196E10 | RCX=packet, RDX=Result<void> hidden return, R8=stream, R9=context。success byte=result+0x40 |
| Output / message copy RVA0x5575D30 / 0x5575B90 | 出力+0x70、messages vector+0x80、successCount+0x98、メッセージstride0x40 |

生の解析結果は `Documents/PEXData/REA/1.26.6005.0/fake-command/` に保存しています。
断片インポートのdecompiler型はMSVC ABIを保証しないため、レジスタと命令列を根拠にしています。
ClientInstanceのsenderと送信経路の対応、実サーバーでの応答は実機確認が必要です。
出力readの旧slot15もslot16に委譲するため、フックはslot16の一箇所です。
元decodeとゲームの結果処理は維持します。フックは共有broker4.5.0 / ABI4で多重購読できます。

プロトコルの意味はMojangの [CommandOriginData (1.26.60)](https://mojang.github.io/bedrock-protocol-docs/1.26.60/types/command-origin-data/) を参照してください。

## Minecraftへのメッセージコマンド登録

`<bedrock/native_message_commands.hpp>` の `NativeMessageCommands` は、
対象ビルドの本物のCommandRegistryへメッセージ引数付きの名前を登録します。

```cpp
bedrock::NativeMessageCommands metadata;
metadata.add("fake", "Send /me with an explicit origin");
// ProfileManagerのadapterでmetadata.load(profile, error)を呼ぶ。
// 以後は所有するクライアントtick上でmetadata.update(client)を呼ぶ。
```

1.26.60.5では `ClientInstance+0x1D0 -> Minecraft+0xC8 -> MinecraftCommands+0x10`
を辿り、既存/meのメッセージ型・ネイティブcallableを検査してから、
CommandRegistry::registerCommandと1引数のオーバーロード登録関数を呼びます。
使用するflagsは元の/meと同じ0x40/0x80です。SyncLocalは設定せず、
要求送信はNativeCommandRequestsで処理します。ゲームの所有領域へSDKのvtableやアロケーターは保存しません。
登録後は再検索してオーバーロードを確認し、同名の既存定義は保持します。
サーバーからコマンド一覧を受け取り直すなどレジストリが変わった場合は再確認します。

静的解析の記録は `PEXData/REA/1.26.6005.0/fake-registration/` に保存しています。
このメタデータ登録はローカル整数コマンドのABIを確定するものではありません。
候補一覧の更新タイミングや実際の送信・応答は実機での確認が必要です。
