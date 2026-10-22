# Minecraftネイティブのキーボード入力

`Keyboard::feed`（1.26.52.3のRVA `0x2D18D0`）は、キーを先に処理する
コールバックです。戻り値はイベントを処理済みにしたかどうかで、ゲームへの
キー注入成功を示しません。この関数だけを呼んでも移動用の入力にはなりません。
Zoom・TabListのキー監視には利用できます。

AutoSprintは`GameCore::keyboardEvent`の本来の処理が完了した後、SDKの
`captureNativeKeyboardEvent`で実際の入力先を取得します。追加Ctrl入力は
`NativeKeyboardInput::send`からMinecraftの`HIDController::onKeyDown`と
`onKeyUp`へ渡します。OSの入力APIやキー状態ポーリングは使いません。

## 1.26.52.3の解析根拠

対象EXEのSHA-256はバージョン別JSONの`target.sha256`と照合します。

| RVA | ネイティブ処理 |
|---|---|
| `0x98FA0` | キーボードイベントのコールバック開始（`.pdata`境界） |
| `0x98FD2` | `0x2D18D0`の事前処理を呼び、処理済みなら末尾へ分岐 |
| `0x99017` | 入力先を取得し、vtable slot 1へキーコードとInputOriginを渡す |
| `0x99030` | 同じ入力先のvtable slot 2へキー解放を渡す |

コールバックの第3引数が指すコンテキストから、ownerは`+0`、イベント種別は
`+16`、キーコードは`+24`で取得されます。入力先はownerの`+64`です。
この配置と仮想関数slotは`native_keyboard`としてバージョン別JSONに保存し、
DLLへ固定せず対応する定義から読みます。新バージョンではAOBの一致に加えて
この配置とABIを再確認してください。自動移植した候補だけでは有効にしません。

SDKのdirect hook経由でコールバックを登録するため、MinHookの所有者は既存Modと
共通です。停止時は自動入力を無効にし、pinされたDLL内のコールバックを素通しで
残します。実行中のコールバックが使用するtrampolineを解放しません。

ローカルの偽HIDControllerを使った回帰テストでnative routeとABIを確認します。
実ゲームでのスプリント成立は別途動作確認が必要です。
