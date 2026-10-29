# PacketHook / Packet IDでの読み書き

## Freecamの移動パケット

Freecamは共通の `BDS_HOOK_PACKET_WIRE_SEND` を購読します。
ここにはバッチ化・圧縮・暗号化より前の「Packet header + payload」が届きます。
Packet IDを通信ヘッダーから読み、ID19 / 144を `MovementPackets` で編集します。
移動パケットのC++フィールドオフセット、個別vtable、個別getter/validatorのRVAは不要です。
旧 `NativeMovementPackets` と両バージョンの `PacketSender.motion` は削除しました。

ゲームとの接続には共通送信HookのRVA/ABIとMSVC x64文字列ABIが必要です。
`layouts.PacketWire.batch_append` はSDK内での役割名です。Minecraftのシンボル名は未特定です。
共通Hookはexact-buildの入口ガードと.pdata境界を確認します。
Hook runtime 4.6以上が必要です。古いruntime ownerが既に選ばれたプロセスでは再起動してください。

## SDK API

- `WirePacket`: `id()`, `rawId()`, サブクライアントID、`payload()`, `replacePayload()`。
- `WireReader`: byte / bool / unsigned varint32・64 / zigzag / little-endian float / string / 範囲指定読み取り。
- `WireWriter`: 対応する書き込み。`replacePayload()`で長さが変わる編集も可能です。
- `PacketWireSendEvent`: `sender()`, `packet()`, `cancel()`。
- `PacketWireEvents`: 共通Hookへの購読と同期イベントの配信。

```cpp
// PacketWireEvents::subscribeで登録したBDS_PacketWireArgs callback内:
bool prepared = packets.dispatch(args, [](bedrock::PacketWireSendEvent& event) {
    if (event.id() != bedrock::PacketId::MovePlayer) return;
    bedrock::WireReader reader(event.packet().payload());
    auto runtimeId = reader.varuint(32);
    // 独自の編集はWireWriterでpayloadを組み立て、最後にreplacePayload。
    // 中止する場合はevent.cancel()。
}, next, frame);
// prepared=falseならnextは未実行。呼出元が通過/キャンセルを決めます。
```

コールバックが終わると借用ポインタは無効になります。
編集対象はSDK所有のコピーです。ゲームの文字列やPacketオブジェクトへ直接書き込みません。
未編集のパケットは元の引数を渡します。編集時は一時文字列を同期的に渡し、ゲーム側が
新しい長さを書いてコピーします。圧縮・暗号化・送信自体はゲームが処理します。

## Mod用API

```cpp
#include <bedrock/packets/packet_api.hpp>
#include <bedrock/packets/protocol_codec.hpp>

// Modの寿命を通して保持し、停止時はDllMain外でstop()する。
bedrock::PacketApi packets;
bedrock::ProtocolCodec codec;
auto incoming = packets.onReceive(bedrock::PacketId::PlayerList,
    [&](const bedrock::PacketReadEvent& event) {
        auto records = codec.decode(event.id(), event.payload());
        // recordsは所有コピー。必要ならworkerへ渡せる。
    });
auto outgoing = packets.onSend(bedrock::PacketId::Text,
    [&](bedrock::PacketWireSendEvent& event) {
        auto fields = codec.decode(event.id(), event.packet().payload());
        fields["message"] = "replacement";
        event.packet().replacePayload(codec.encode(event.id(), fields));
        // event.cancel(); で送信をキャンセルできる。
    });
std::string error;
packets.installReceive(selectedProfileJson, error);
packets.installSend(selectedProfileJson, error);
// packets.unsubscribe(outgoing); // 1件だけ解除
// packets.stop();                // 全購読を解除
```

受信はゲームのdecode成功後の観測APIです。受信済みのゲームオブジェクトを書き換えるAPIではありません。
送信はbatch/compression前にコピーを書き換えます。コールバックはnetwork threadの可能性があるため、Actorを直接読まずコピーしたデータを使います。
ID別購読では不要なパケットpayloadをコピーしません。購読追加・解除はcallback一覧のコピーを公開します。

## 通信定義

[PrismarineJSのprotocol.json](https://raw.githubusercontent.com/PrismarineJS/minecraft-data/9c97fe50a82feb2c0f2be1cff35ce19c891bfb35/data/bedrock/1.26.60/protocol.json)
を加工せず`data/protocols/<protocol-version>/protocol.json`へ保存します。
参照するソースは版別manifestの`protocol.path`、`version_path`、`sha256`で選択します。
CMakeが選択版をビルド時に組み込み、SDKは共通名の生成ヘッダーを使います。
PlayerList専用JSONや版名付きのC++ヘッダーは作りません。

`ProtocolCodec`は同じ完全なschemaを直接参照してdecode/encodeします。
container、array、mapper、switch、string/buffer、UUID、整数、zigzag、lf32等に対応します。
NBT、bitflags等の未対応の特殊型は名前付きエラーになり、推測して書き込みません。
未対応部分は`WireReader` / `WireWriter`で生payloadを扱えます。wire enumと完全なschemaは同じdataから生成します。

`PlayerListPackets`はPlayerListとPlayerSkinを解析し、UUIDをキーに追加・削除・顔画像更新を保持します。
StartGame／Disconnectでクリアします。途中ロード前の初回名簿は再現できないため、受信できていなければワールドへ入り直してください。
不正なpacketは以前の名簿を保持し、拒否件数と理由を診断へ残します。

`MovementPackets` は通信上の順序で読みます。InputDataはzigzag32 enumの配列です。
位置・移動ベクトル・delta・analog/raw入力を編集し、teleport ackなどの非移動フラグを保持します。
transaction / stack request / block actionの可変長部分は元のバイト列をそのまま保持します。
ネストしたアイテムデータの型付き編集はこのcodecの対象外です。
移動のreset/teleport/rotation、vehicle、不明なフラグ、壊れたデータは編集しません。
Freecam有効中の編集失敗は送信を止め、Freecam解除・本体復帰へ進みます。

## 実機確認の範囲

この経路は静的解析に基づく候補で、実機の位置固定・解除復帰は未確認です。
FreecamはsendToServerの同期呼出しに帰属するwireイベントだけを編集します。
別スレッドへ遅延した送信、サーバー側送信、受信パケットをクライアント送信と推定しません。
受信Hookは今回のAPIに追加していません。既存のFake応答経路は従来のCommandOutput read Hookです。

`Freecam.log` の5秒ごとのsnapshotに次を出します。

| 項目 | 意味 |
| --- | --- |
| wire_packets | 対象クライアント送信に帰属した通信パケット数 |
| wire_without_sender | sendToServerの同期スコープ外の呼出し数 |
| movement_packet_kinds | 読み取れた移動パケット種別（Move=1 / Auth=2） |
| wire_movement_rejected | 姿勢読み取りできなかった移動パケット数 |

`movement_packet_kinds=0` の間はFreecamを有効化しません。
Fake等の既存ネイティブAPIとActor/Cameraのレイアウトは移動パケットの通信codecとは別に残ります。
