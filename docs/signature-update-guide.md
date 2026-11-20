# シグネチャ更新ガイド

この文書は、`data/<version>/windows-x64/` 内の関数・オフセット・vtable 定義を新しい Minecraft Bedrock Windows x64 EXE に更新する際の入口ガイドです。詳細な手順は [version-update-workflow.md](version-update-workflow.md) を参照してください。

## 前提

- 対象の `Minecraft.Windows.exe` は `local-binaries/` に配置し、リポジトリにはコミットしません。
- 作業環境: `bedrock-symbols`、`Bedrock Lens`（Ghidra 連携）、`build-linux.sh` または GitHub Actions。

## 更新の流れ

1. **EXE の身元確認**
   - `uv run bedrock-symbols inspect <NEW_EXE>` で SHA-256、`.text` 範囲、`.pdata` 関数数を確認します。
   - 同じバージョン表記でもハッシュが異なる場合は別プロファイルとして扱います。

2. **旧 signature の自動移植**
   - `bedrock-symbols port` で masked AOB による候補を生成します。
   - 生成されたシンボルはすべて `candidate` です。`verified` への昇格は ABI 確認後に行います。

3. **構造的類似で補完**
   - `bedrock-symbols client-profile` で Latite 由来パターンを取得します。
   - Bedrock Lens / Ghidra / BSim で AOB 未ヒット関数を特定します。

4. **最終プロファイル作成**
   - `data/<NEW_VERSION>/windows-x64/manifest.json` を作成します。
   - 各シンボルに `rva`、`signature`、`entry_check`、`status`、`source` を残します。
   - ABI を確認した関数だけ `status: verified`、`abi_verified: true` に昇格します。

5. **JSON / manifest 検証**
   - `python tools/profile_parts.py refresh data/<NEW_VERSION>/windows-x64/manifest.json`
   - `bedrock-symbols index data --output data/manifest.json`

6. **ビルドと runtime 検証**
   - `./build-linux.sh`（または `--stage-a`）で対象版をビルドします。
   - BedrockOnLinux などで注入し、必要な Mod 動作を確認します。
   - プロファイルキャッシュ `%LOCALAPPDATA%\PEXData\BedrockDynamicSDK\profile-cache\` を適宜削除します。

## 遵守事項

- 共通の `Class::function` 識別名を維持します。
- 変更は `data/<version>/windows-x64/classes/*.json` と SDK アダプターに入れ、Mod に RVA・offset を追加しません。
- レイアウト内の「リスト形式の RVA」（vtable 候補リスト等）は、新旧プロファイルの全 RVA を棚卸しして古い `.rdata/.data` を指したままの値が残っていないか確認します。

## 関連資料

- 詳細手順: [version-update-workflow.md](version-update-workflow.md)
- 保守規約: [profile-maintenance.md](profile-maintenance.md)
- SDK 構造: [sdk-structure.md](sdk-structure.md)
- CheckSig 調査: [checksig-jobs.md](checksig-jobs.md)
