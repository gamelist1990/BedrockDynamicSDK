# EffectFake

クライアント側だけでポーション効果を付ける Mod（BedrockDynamicSDK の TS プラグイン）。

サーバーから付けられた効果ではないので、サーバー側には一切影響しません。自分への効果と見た目だけが変わります。

## コマンド

バニラの `/effect` と同じ形です。全引数に補完が付きます。

```
/effectfake give <effect> [duration] [amplifier] [visible]
/effectfake clear [effect]
```

| 引数 | 説明 |
|---|---|
| `effect` | エフェクト ID（例: `speed`, `night_vision`）。補完あり |
| `duration` | 秒数。`Infinity` で無限。省略時は 30 秒。補完あり |
| `amplifier` | 強さ 0–255。省略時は 0。補完あり |
| `visible` | パーティクル表示 `true` / `false`。省略時は `true`。補完あり |

`clear` は引数なしで全消し、エフェクト ID を付けるとその効果だけ消します。

例:

```
/effectfake give night_vision Infinity
/effectfake give speed 60 2 false
/effectfake clear speed
/effectfake clear
```

## 導入

このフォルダを BedrockSDK の `mods/` フォルダに入れると、ゲーム起動時に自動で読み込まれます（`/mods list` で確認、`/mods reload EffectFake` で再読み込み）。

## 注意

- 効果はクライアント側のみです。再ログインやワールド移動で消える場合があります。
- サーバー側の処理（ダメージ計算など）はサーバーが判断するため、サーバーによっては見た目だけになる効果もあります。
