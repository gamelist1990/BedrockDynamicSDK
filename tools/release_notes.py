#!/usr/bin/env python3
"""Generate a readable GitHub Release body + SHA256SUMS.txt for a version's
mod ZIPs.

The release job downloads every `release-<Mod>-<version>` artifact into one
directory; this script inventories the ZIPs (name, size, SHA-256), renders a
Markdown body with a per-Mod table, install notes, and the changes since the
previous build, and writes a sha256sum-compatible checksums file.

Usage:
    release_notes.py --dir release --version 1.26.65.1 --sha <full sha> \
        --repo owner/name --changes changes.txt --out notes.md \
        --sums SHA256SUMS.txt [--prerelease]
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


def human_size(n: int) -> str:
    size = float(n)
    for unit in ("B", "KiB", "MiB", "GiB"):
        if size < 1024 or unit == "GiB":
            return f"{size:.0f} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1024
    return f"{size:.1f} GiB"


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True, type=Path)
    ap.add_argument("--version", required=True)
    ap.add_argument("--sha", required=True)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--changes", type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--sums", required=True, type=Path)
    ap.add_argument("--prerelease", action="store_true")
    args = ap.parse_args()

    suffix = f"-{args.version}-windows-x64.zip"
    zips = sorted(args.dir.glob(f"*{suffix}"))
    if not zips:
        raise SystemExit(f"no release ZIPs matching *{suffix} in {args.dir}")

    rows = []
    sums = []
    for z in zips:
        mod = z.name[: -len(suffix)]
        digest = sha256_of(z)
        rows.append((mod, z.name, human_size(z.stat().st_size), digest))
        sums.append(f"{digest}  {z.name}")
    args.sums.write_text("\n".join(sums) + "\n", encoding="utf-8")

    short = args.sha[:7]
    commit_url = f"https://github.com/{args.repo}/commit/{args.sha}"
    lines = [
        f"## Minecraft {args.version} 用 DLL セット (Windows x64)",
        "",
        f"ビルド: [`{short}`]({commit_url})",
    ]
    if args.prerelease:
        lines += ["", "> プレリリース版です(旧バージョン向け)。"]
    lines += [
        "",
        "## 含まれる Mod",
        "",
        "| Mod | ファイル | サイズ | SHA-256 |",
        "| --- | --- | ---: | --- |",
    ]
    for mod, name, size, digest in rows:
        lines.append(f"| {mod} | `{name}` | {size} | `{digest[:12]}…` |")
    lines += [
        "",
        "全ファイルの完全なチェックサムは添付の `SHA256SUMS.txt` を参照してください。",
        "",
        "## 使い方",
        "",
        "1. 使いたい Mod の ZIP をダウンロードして展開します。",
        "2. DLL と `profile` フォルダ一式を、利用中のローダー/注入先に配置します"
        "(ZIP 内の `build-info.json` にビルド情報が入っています)。",
        "3. Mod ごとの詳細は ZIP 内の README を参照してください。",
    ]
    changes = []
    if args.changes and args.changes.is_file():
        changes = [ln.rstrip() for ln in args.changes.read_text(encoding="utf-8").splitlines() if ln.strip()]
    if changes:
        lines += ["", "## 前回ビルドからの変更", ""]
        lines += [f"- {ln}" for ln in changes[:30]]
        if len(changes) > 30:
            lines.append(f"- …ほか {len(changes) - 30} 件")
    lines += [
        "",
        "## 注意",
        "",
        "機能ごとの検証範囲は README を参照してください。"
        "ネイティブ ABI やレイアウトには候補(candidate)ステータスのものが含まれる場合があります。",
        "",
        f"<!-- build-commit: {args.sha} -->",
        "",
    ]
    args.out.write_text("\n".join(lines), encoding="utf-8")
    print(f"notes for {len(rows)} mods -> {args.out}; checksums -> {args.sums}")


if __name__ == "__main__":
    main()
