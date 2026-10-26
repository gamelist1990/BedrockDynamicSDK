# Top — 安全な上方テレポート

Minecraft **1.26.52.3 / Windows x64 / MSVC Release** のSDK example DLLです。
新しいネイティブ定義は候補であり、実ゲームでの動作確認はこれからです。

## コマンド

| コマンド | 動作 |
|---|---|
| `/top` | 頭上のブロックを越えた、最も近い安全な着地点へ移動 |
| `/top 3` | 現在の足元から正確に3ブロック上だけを判定して移動 |
| `/top N` | 正の整数Nブロック上。0・負数・小数・余分な引数はエラー |

X/Zは維持します。引数付きで移動先が安全でなければ、近くの別座標へずらしません。
失敗するとチャットへ **「テレポートできません！ 理由」** を表示します。

## 安全判定

- 現在のプレイヤーのAABB幅・高さを使い、隣のマスへはみ出す部分も検査。
- 頭・体が入る全マスは空気であることが必要。液体、植物なども保守的に拒否。
- 足元の全幅に、既知の通常の立方体ブロック（石・土・原木・板材など）が必要。安全性未確認の種類や空中への移動は拒否。
- ハーフ・階段・柵など不完全な床、危険な床、独自名前空間の安全性未確認の床は拒否。
- 未読み込み・読み取り失敗・高さ範囲外は拒否。取得失敗を空気として扱いません。
- 実行直前に再検査。探索中の移動・姿勢変化、RuntimeID・ワールド・BlockSource変更は中止。
- 移動時は現在位置・前回位置・AABBを同じだけ動かし、速度と落下距離をリセット。

探索の上限は既定で無制限です。ただしディメンション本来の高さ範囲と、読み込まれたブロックの範囲内で動作します。
スキャンはtickごとに分割。待機中はActor/ECS/ブロックを走査せず、ワーカーはログ保存とHook登録だけを行います。
単一のネイティブ呼び出しが協調時間予算を超える可能性はあります。

## 設定

`Documents/PEXData/Top/config.json`（WindowsのDocuments既知フォルダ）:

```json
{
  "enabled": true,
  "max_rise": 0,
  "scan_levels_per_tick": 8,
  "cooperative_budget_us": 500
}
```

`max_rise=0` は独自の距離上限なし。正数で上限を設定できます。設定変更は再起動後に反映します。
ログは同じ場所の `Top.log`。起動後にEscで一度メニューを開いて戻ると、既存SDKのClientInstance取得が有効になります。

## コマンド登録の仕組み

SDKの `bedrock::ClientCommands` に `commands.add("top", handler)` で登録。
送信直前のCommandRequestPacketで登録済みコマンドだけを消費し、コピーした文字列をメインtickで処理します。
未登録のコマンドは本体へ渡します。サーバーへ `/top` コマンドを送りません。
LeviLaminaのネイティブCommandRegistryへの直接登録とは別のクライアント内ディスパッチ方式です。
標準コマンド候補補完への追加は実装していません。

マルチプレイでの最終位置はサーバーにより補正される場合があります。移動制限を解除するパケット処理はありません。
Freecamを停止してから使ってください。DLLのアンロードはサポートせず、差し替えはMinecraft完全終了後に行います。

## ビルド

```powershell
cmake -S exsample/Top -B build-top -A x64 "-DFETCHCONTENT_SOURCE_DIR_BEDROCK_SDK=$PWD"
cmake --build build-top --config Release --parallel 2
```

`build-top/Release/BedrockTop.dll` を生成します。
共通のガード付きECS/BlockSource読み取りをSpeedBuilder exampleから再利用しますが、Title Hook・配置・アイテム操作は使いません。

参考: [Borion TopCommand](https://github.com/Borion-Updated/Borion-OSS/blob/master/Horion/Command/Commands/TopCommand.cpp)、
[LeviLamina CommandRegistrar](https://github.com/LiteLDev/LeviLamina/blob/main/src/ll/api/command/CommandRegistrar.cpp)。
