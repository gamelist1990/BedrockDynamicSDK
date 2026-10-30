# リソースパックAPIの保守

公開窓口は `bedrock::NativeResourcePacks`（`bedrock/native/resource_packs.hpp`）です。Modは `ProfileManager` で `Feature::ResourcePacks` を準備し、`load`、`install` を呼びます。ログを受け取る場合は `setTraceCallback`、状態のコピーは `status` を使います。Modからネイティブ関数や版別オフセットを指定しません。

| 場所 | 責務 |
| --- | --- |
| `api/resource_packs.hpp` | グローバルリソース画面で適用を許可する共通ポリシー |
| `src/native_resource_packs.cpp` | 16個のHook、ネイティブ所有の選択・適用経路、完了待ちと描画更新 |
| `detail/pending_terrain_refresh.hpp` | 要求世代、アトラス完了、実行中の要求保持と再試行。ポインターは識別用 |
| `detail/resource_reload_barrier.hpp` | 完了した通常フレームと要求・ローダー・アトラスの同一性確認 |
| `detail/native_material_uvs.hpp` | 版別設定を起動時に数値構造体へ変換し、カスタムUVを読み取り・検査・反映 |
| `data/<version>/windows-x64/classes/ResourcePackManager.json` | 関数のABI・入口ガード・RVA、フィールド配置と検証記録 |

適用はネイティブの選択変更 → ネイティブ所有の適用コールバック → スタック合成 → アトラス完了 → ローダー完了とフレーム境界確認 → 描画タスク待機後の再確認 → ブロック/UV/モデル参照更新 → カメラ更新の順です。設定終了は必須ではありません。前回の追加更新が未完了なら次の適用コールバックをゲーム側に保持したまま待機します。

要求は更新が完了するまで保持します。描画タスク待機後にロード中へ戻った場合は、要求を再試行可能に戻して完了フレームの確認をやり直します。世界の変更は中止します。古い世代の完了・中止が新しい要求を消費しないようにします。ネイティブの画面やコールバックを独自に再構築してはいけません。

v21の検証はユーザー報告に基づき、`analysis/mod_runtime_validation.json` と使用中のシンボルに記録しています。新しい待機処理の実機検証状況は `load_completion` に分けています。これを全SDK、他バージョン、削除済みの全体再初期化処理の検証へ拡張してはいけません。識別名に残る `Candidate` 接尾辞は既存参照との安定性のため保持します。現在の検証状態は名前ではなくメタデータを参照してください。

次のゲーム版では [プロファイル更新手順](profile-maintenance.md) に従い、正確なEXE指紋・呼出規約・入口と呼出元ガード・構造体配置を再確認します。同じABIなら主にクラス定義を更新し、ABIが変わればSDKのアダプターも変更します。別版のRVAへ自動復帰する処理を追加しません。現在の対応ABIは `windows-live-reload-v21-load-complete` で、互換性のない古いプロファイルは拒否し、同一EXEの内蔵定義への補完経路に任せます。

廃止した停止・復帰、全体形状再初期化、クライアント資源全体の通知、独自コールバック消費用ヘルパーはアクティブ定義から削除しました。過去の根拠はGit履歴とREA記録に残っています。`Status` の旧カウンター `platformCycles`、`suspendGateOverrides`、`geometryReloaded`、`clientDefinitionsReloaded` は既存コードとの互換性のため0を返します。`nativeScreenWaitFrames` は `completionWaitFrames` の互換エイリアスです。
