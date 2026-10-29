# PacketHookとSDKのパケットアクセス

## 送信の共通経路

`BDS_HOOK_PACKET_SEND_TO_SERVER` はプロセス内で1本のHookを共有します。
各Modは同じ入口を購読し、`NativePacketEvents` でID付きの借用イベントを受け取ります。
変更・キャンセルは送信前のコールバック中に完了させます。ポインタを別スレッドへ保持しません。

```cpp
bedrock::NativePacketEvents packets;
// exact-build ProfileManager adapter内で packets.load(parseNativeProfile(profile.json()))
// 共有送信Hookのcallback内:
packets.dispatch(args, [](bedrock::PacketSendEvent& event) {
    if (event.id() == bedrock::PacketId::PlayerAuthInput) {
        // SDKのtyped accessorで参照・編集。event.cancel()で送信を中止。
    }
}, next, frame);
```

`NativePacketView` は実際のPacket vtableからgetId / isValidを呼びます。
個別Packetのvtable RVA・getter RVA・validator RVAを起動条件にしません。
共通ABIのslotとモジュール内関数ポインタを検査し、native呼出しはSEHで保護します。
送信入口そのものの取得とHook対象の正確な関数入口確認は引き続き必要です。

## プロトコルとネイティブレイアウト

| 情報 | 使用する根拠 |
| --- | --- |
| Packet ID、通信上のフィールド名・型、InputDataの値 | PrismarineJS/minecraft-data 1.26.60 |
| Packet共通vtable ABI、C++フィールドのオフセット | 対象EXEの解析とバージョン別windows-x64.json |
| 編集方針、元の値の保存と復元 | SDK NativeMovementPackets |

参照元: https://github.com/PrismarineJS/minecraft-data/tree/9c97fe50a82feb2c0f2be1cff35ce19c891bfb35/data/bedrock/1.26.60

`data/protocols/bedrock-1.26.60.json` は選択した4パケットの通信定義、InputData、
ソースcommit/SHA256を保持します。`tools/import_bedrock_protocol.py` が
SDK用のID・フラグ定数も再生成します。通常のDLLビルドと実行時に外部取得は不要です。

この通信定義のInputDataは配列ですが、対象ネイティブ実装は128bitの集合です。
SDKは確認済みのenum値とネイティブ保存形式を明示的に対応させます。

## Freecam

FreecamはID19と144だけを処理し、座標・フラグのオフセットをMod内に持ちません。
SDKが元の姿勢を読み、位置固定・移動入力の解除を行います。
teleport acknowledgement、item interaction、block action、stack requestとそれに対応する所有データは保持します。
編集の前後にネイティブ検証を通し、不成立なら変更したスカラー値を戻します。
呼出元は編集失敗したパケットをキャンセルし、Freecamの解除・本体復帰へ進みます。

1.26.52.3で使う移動フラグの集合は従来実装から変更していません。
1.26.60.5での実機の送信・位置固定・解除復帰は引き続き確認が必要です。
