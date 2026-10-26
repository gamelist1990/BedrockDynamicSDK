# Top — 安全な上方テレポート

Minecraft **1.26.52.3 / Windows x64 / MSVC Release** のSDK example DLLです。
revision2は本体のCommandRegistryへ登録して候補表示・Tab補完・引数ガイドを接続します。
新しいネイティブ定義は候補であり、今回のDLLでの実ゲーム確認はこれからです。

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

SDKの既存ディスパッチに加え、本体の **CommandRegistry** に次を登録します。

- 名前 `top`、説明、省略可能な整数引数 `blocks`。
- 本体Helpと同じ `SyncLocal` / `NotCheat` フラグ。
- `/t`・`/to` で標準の候補一覧に `top`、Tab補完、`/top [blocks: int]` の引数ガイドを表示するための構文情報。
- ネイティブの実行コールバックは文字列をキューへコピーするだけ。安全判定と移動は次のメインtickで行います。

本体の登録・引数作成・オーバーロード登録関数を使い、名前や配列のメモリは本体側で確保します。
独自Commandインスタンスは固定したDLL側で確保し、仮想デストラクタも同じDLLで処理します。
ワールド／レジストリ変更後は再確認し、欠けた場合は再登録します。確認は20tickごとです。
登録に成功するとログへ次の行が出ます。

```text
/top native CommandRegistry registered [SyncLocal + NotCheat; blocks: optional int; native autocomplete + local execution]
```

旧版の送信前パケット処理もフォールバックとして残します。ネイティブ実行とパケットの両方を通る場合は同じ要求を二度実行しません。
未登録のコマンドは本体へ渡し、既存の同名コマンドがある場合はその構文情報を上書きしません。
候補登録がガード不一致などで失敗した場合は、その理由を `Top.log` に記録します。
標準の候補一覧が出るか、レジストリ更新後も維持されるかは実機確認が必要です。

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
