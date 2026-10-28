# Fake.dll

`/fake メッセージ` をローカルで受け付け、`CommandRequestPacket (77)` のコマンドを
`/me メッセージ`、`CommandOriginData.type` を **AutomationPlayer = 5** として送ります。
通常のコマンドはそのままゲームへ渡します。

## 入手・起動

1. バージョン別リリースから **Fake-1.26.60.5-windows-x64.zip** を取得します。個別ビルドはCIの **Build DLLs and releases** でversionとmods=Fakeを指定します。
2. ZIPを展開し、既存Modと同じ方法で `Fake.dll` をゲームへ読み込みます。
3. ワールド内でEscを開いて戻り、ClientInstanceを一度観測させてください。
4. `Documents/PEXData/Fake/Fake.log` で `/fake registered in Minecraft CommandRegistry` と `Fake ready` が出た後に実行します。
5. コマンド画面を開いていた場合は一度閉じ、開き直して候補を確認してください。

以前のFake.dllはプロセス終了まで保持されるため、更新時はMinecraftを完全終了してから新しいDLLを読み込んでください。

対象は **Windows x64 / 1.26.60.5、EXE SHA-256
988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a** です。
定義はSDKのProfileManager経由でGitHub・キャッシュから取得します。
新しい送受信レイアウトはREA/Ghidraの静的候補です。実ゲームでの送信・応答は未確認です。

## コマンド

```text
/fake こんにちは
/fake --origin 5 こんにちは
/fake --origin=0 通常のPlayer Originで送信
/fake -o 6 ClientAutomationで送信
/fake --origin 255 任意の数値
/fake -- --origin という文字列をメッセージにする
```

Originオプションはメッセージの前に指定します。省略時は5です。
型は1バイトなので **0〜255の整数**を受け付け、範囲外・負数・小数は拒否します。
メッセージ内の空白・日本語を保持します。空メッセージ・改行・NULは拒否します。
上限はUTF-8で2044バイト（`/me ` を含む要求全体では2048バイト）です。
ゲームの `CommandRegistry` に `/fake <message>` を登録します。
対象バージョンの `/me` と同じメッセージ引数の定義・ネイティブfactoryを、ゲーム自身の登録関数でコピーします。
`/fake` の送信要求をModで受け取り、Origin付きの `/me` 要求を組み立てます。
既存の同名コマンドは上書きしません。ネイティブ整数コマンドの登録APIとは別です。
登録関数・レイアウトは静的解析に基づく候補で、ゲーム内の候補表示と送受信は実機確認が必要です。

| 値 | 名前 | 値 | 名前 |
| --- | --- | --- | --- |
| 0 | Player | 8 | Entity |
| 1 | CommandBlock | 9 | Virtual |
| 2 | MinecartCommandBlock | 10 | GameArgument |
| 3 | DevConsole | 11 | EntityServer |
| 4 | Test | 12 | Precompiled |
| 5 | AutomationPlayer | 13 | GameDirectorEntityServer |
| 6 | ClientAutomation | 14 | Scripting |
| 7 | DedicatedServer | 15 | ExecuteContext |

## 結果

結果は `Fake.log` にJSONで記録します。

- `status: received`: UUIDとRequestIdが一致する `CommandOutputPacket (79)` を受信。
- `success_count`: サーバーが返した成功件数。`succeeded` は受信済みかつ成功件数が1以上の場合のみtrue。
- `messages`: 成否、翻訳キーまたは本文の `id`、文字列の `parameters`。
- `output_type` / `has_data_set`: 出力種別と追加データの有無。PropertyBag本体の解析は非対応。
- `status: timeout`: 10秒以内に応答を確認できなかった状態。成功・失敗は断定しません。
- `unavailable` / `disconnected` / `cancelled`: 未送信、ワールド切替、取消。

サーバーの設定やOriginによっては結果パケットが返らないことがあります。
`then` の通知はゲームのtick上で行うため、tick停止中の通知は再開後になります。
ワールド・ClientInstanceが切り替わった場合は次のtickで保留要求を終了します。
フック所有者が4.5.0未満の場合はMinecraftを再起動し、現在のDLLを読み込んでください。

SDKの共通APIの使い方は [command-requests.md](../../docs/command-requests.md) を参照してください。
`Fake_Stop()` で停止、`Fake_GetStatus()` で状態を取得できます
（0未開始 / 1初期化・登録または送信準備待ち / 2登録と送信準備完了 / 3停止 / 4初期化失敗）。DLLはプロセス終了まで保持します。

## 登録・起動ログ

| ログ | 意味 |
| --- | --- |
| `Fake attached; waiting ...` | DLLのフック初期化完了。まだコマンド登録・送信の準備完了ではない |
| `registration waiting: ClientInstance` | Escを開閉し、クライアントを一度観測する必要がある |
| `registration waiting: Minecraft CommandRegistry` | レジストリを取得できていない |
| `registration waiting: server /me message definition` | 元にする/me定義がレジストリにない。ワールド・サーバー接続を確認 |
| `registration blocked: ...` | 同名の衝突やレイアウト不一致。具体的な理由を記録 |
| `/fake registered in Minecraft CommandRegistry ...` | ゲーム側で登録した定義を再検索し、メッセージのオーバーロードも確認 |
| `sender waiting: ...` / `sender blocked: ...` | 送信経路が使えない理由 |
| `Fake ready; ...` | コマンド登録と送信フックの両方を確認 |

レジストリの置き換えはゲームtickで再確認します。停止後もゲーム所有の候補定義はレジストリが再作成されるまで残る場合があります。
