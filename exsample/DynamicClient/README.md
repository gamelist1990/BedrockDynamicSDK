# DynamicClient

Minecraft Bedrock **1.26.65.1 / Windows x64** 向けの単一 DLL です。Zoom、AutoSprint、TabList、Fullbright、Freecam を統合します。

## 使い方

1. Minecraft のタイトル画面が表示された後に `DynamicClient.dll` だけを読み込んでください。別の Hook Runtime DLL や旧個別 Mod は不要です。
2. 初回は **プレイ** を一度開き、ワールドに入らずタイトルへ戻ります。
3. **設定 → Mod Menu → DynamicClient** から機能を切り替えます。設定は `%LOCALAPPDATA%\BedrockDynamicSDK\ModSettings\mod-dynamic-client.json` に保存されます。旧個別 Mod の JSON は読み込みません。
4. **設定 → キーボード＆マウス** の最下部にある **Mod キー設定** で Zoom、TabList、Freecam の割り当てを変更できます。「キーを変更」を押してから新しいキーを押します。入力待ち中の Esc は、その割り当てだけを初期値に戻します。割り当ては `mod-dynamic-client-keybinds.json` に保存されます。

| 機能 | 操作 |
| --- | --- |
| Zoom | 初期値 `C`。押下中／切替を選べます。ホイールによる倍率変更はセッション中のみ有効です。 |
| AutoSprint | `W` で走り、`Shift` で一時停止します。 |
| TabList | 初期値 `Tab`。押している間だけ表示します。空の場合はワールドへ入り直してください。 |
| Fullbright | 設定で有効にすると表示用ガンマだけを上書きします。既定はオフです。 |
| Freecam | 既定はオフ。許可された環境でのみ設定を有効にし、ゲーム中に割り当てキー（初期値 `M`）で切り替えます。`WASD`、Space／Shift で移動、Ctrl で加速します。地下表示は実験的です。 |

**Freecam は、許可されていないサーバーでは BAN される可能性があり、保証はしない。自己責任で使うこと。** 利用先のルールを確認してください。

`DynamicClient.log` と TabList／Freecam の個別ログは DLL と同じディレクトリに出力されます。OreUI の接続ログは `%LOCALAPPDATA%\BedrockDynamicSDK\ModSettings\oreui.log` です。設定画面を最初に開いて Mod Menu が出ない場合、ゲームを再起動し、先に **プレイ** を開いてください。

## ビルド

SDK ルートから `./build-linux.sh --stage-a` を実行するか、Windows で `cmake -S exsample/DynamicClient -B build-dynamic-client -A x64` の後に `cmake --build build-dynamic-client --config Release` を実行してください。

Zoom 由来のソースは同梱の `LICENSE.Zoom` を参照してください。
