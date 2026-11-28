#!/usr/bin/env python3
"""Build the public quick reference with the standard library only."""

import argparse
from html import escape
from pathlib import Path
import re
from shutil import copyfile

from ts_pages import FIRST as TS_FIRST, PAGES as TS
from cpp_pages import PAGES as CPP
from signature_shapes import CPP_SHAPES, TS_SHAPES

ROOT = Path(__file__).resolve().parents[2]
SITE = ROOT / "docs/site"
TYPES = ROOT / "sdk/ts/node_modules/@bedrock/sdk/index.d.ts"
GITHUB = "https://github.com/gamelist1990/BedrockDynamicSDK"
GROUPS = (("TypeScript", "ts", TS), ("C++", "api", CPP))
SECTION_SUMMARIES = {
    "ts": "ScriptClient で動く TypeScript Mod の API です。目的に合うページから、使い方と最小例を確認できます。",
    "api": "C++20 の DLL Mod 向けリファレンスです。SDK とホストの使い方から、用途別の API まで確認できます。",
}
FONTS = (
    "https://fonts.googleapis.com/css2?family=Plus+Jakarta+Sans:wght@400;500;700;800"
    "&family=Space+Grotesk:wght@500;700&display=swap"
)

TOKEN = re.compile(
    r'//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|`(?:\\.|[^`\\])*`|'
    r'\b(?:import|from|export|declare|const|let|if|return|true|false|null|undefined|'
    r'void|bool|boolean|number|string|class|struct|enum|namespace|using|public|'
    r'private|readonly|interface|type|static|virtual|override|noexcept|auto|'
    r'new|throw|unsigned|double|float|function|async|await|template|typename)\b'
)


def code(text, language, label="コード"):
    text = text.strip()
    longest = max((len(line) for line in text.split("\n")), default=0)
    fit = (' class="fit-long"' if longest <= 64 else ' class="fit-wrap"') if longest > 52 else ""
    parts, pos = [], 0
    for match in TOKEN.finditer(text):
        token = match[0]
        style = "comment" if token.startswith("//") else "string" if token[0] in "\"'`" else "keyword"
        parts.extend((escape(text[pos:match.start()]), f'<span class="{style}">{escape(token)}</span>'))
        pos = match.end()
    parts.append(escape(text[pos:]))
    return (f'<pre{fit} tabindex="0" role="region" aria-label="{escape(label)}">'
            f'<code class="language-{language}">{"".join(parts)}</code></pre>')


def ts_signatures(selectors, source=TYPES):
    """Copy named declarations; Name:member,member limits an interface/class."""
    text = re.sub(r"(?m)^ *(/\*\*[\s\S]*?\*/\n)", "", source.read_text(encoding="utf-8"))
    starts = list(re.finditer(
        r"(?m)^(?:export )?(?:declare )?(?:interface|type|class|const) (\w+)\b",
        text,
    ))
    declarations = {}
    for i, match in enumerate(starts):
        end = starts[i + 1].start() if i + 1 < len(starts) else len(text)
        declarations.setdefault(match[1], []).append(text[match.start():end].strip())
    snippets = []
    for selector in selectors.split():
        name, _, members = selector.partition(":")
        value = "\n".join(declarations[name])
        if members:
            lines = value.splitlines()
            wanted = set(members.split(","))
            selected = [
                (line, re.match(r"  (?:(?:readonly|get) )?(\w+)", line))
                for line in lines[1:-1]
            ]
            found = {match[1] for _, match in selected if match and match[1] in wanted}
            if found != wanted:
                raise ValueError(f"{name}: missing members {wanted - found}")
            value = "\n".join([lines[0], *(line for line, match in selected if match and match[1] in wanted), lines[-1]])
        snippets.append(value)
    return "\n\n".join(snippets)


def signature_table(name, rows):
    if not rows:
        return ""
    body = "".join(
        f'<tr><th scope="row"><code>{escape(field)}</code></th><td><code>{escape(type_)}</code></td>'
        f'<td>{escape(required)}</td><td>{escape(description)}</td></tr>'
        for field, type_, required, description in rows
    )
    headings = "".join(f'<th scope="col">{column}</th>' for column in ("名前", "型", "必須", "説明"))
    return (f'<div class="table-scroll" tabindex="0" role="region" aria-label="{escape(name)} の項目">'
            f'<table><thead><tr>{headings}</tr>'
            f'</thead><tbody>{body}</tbody></table></div>')


def signature_group(name, text, language, rows=(), note="", *, heading="h3"):
    shape = code(text, language, name) if text else ""
    hint = f'<p class="signature-note">{escape(note)}</p>' if note else ""
    return (f'<div class="signature-group"><{heading}><code>{escape(name)}</code></{heading}>'
            f'{shape}{hint}{signature_table(name, rows)}</div>')


def ts_reference(selectors):
    groups = []
    for selector in selectors.split():
        name = selector.partition(":")[0]
        if selector in TS_SHAPES:
            shape, rows, note = TS_SHAPES[selector]
            groups.append(signature_group(name, shape, "ts", rows, note))
            continue
        text = ts_signatures(selector)
        text = re.sub(r"(?m)^export (?:declare )?", "", text)
        text, constructors = re.subn(r"(?m)^  (?:private|protected) constructor\(.*\);\n", "", text)
        if constructors or name == "Player":
            # These are instance shapes supplied by the SDK, not constructors to call.
            text = text.replace("class ", "interface ", 1)
            text = re.sub(r"(?m)^  get (\w+)\(\):", r"  readonly \1:", text)
        # Give compact source interfaces one field per line without changing their types.
        text = re.sub(r"(?m)^(interface \w+ \{) (.+) \}$",
                      lambda m: m[1] + "\n  " + ";\n  ".join(m[2].rstrip(";").split("; ")) + ";\n}",
                      text)
        groups.append(signature_group(name, text, "ts"))
    return "\n".join(groups)


def cpp_reference(slug, signatures):
    shapes = CPP_SHAPES.get(slug, {})
    heading = "h4" if slug == "index" else "h3"
    groups = []
    for text in signatures.strip().split("\n\n"):
        title, _, rest = text.partition("\n")
        if title.startswith("// "):
            groups.append(signature_group(title[3:], rest, "cpp", heading=heading))
        else:
            for declaration in re.split(r"\n(?=struct |enum class |using )", text):
                name = declaration.split()[2 if declaration.startswith("enum class ") else 1]
                if name in shapes:
                    example, rows, note = shapes[name]
                    groups.append(signature_group(name, example, "cpp", rows, note, heading=heading))
                else:
                    groups.append(signature_group(name, declaration, "cpp", heading=heading))
    return "\n".join(groups)


def section_cards(folder, pages):
    cards = []
    for slug, (title, summary, *_) in pages.items():
        # Card descriptions are already inside a link; strip any embedded links.
        summary = re.sub(r"</?a\b[^>]*>", "", summary)
        href = "#sdk-host" if folder == "api" and slug == "index" else f"{slug}.html"
        cards.append(f'<a class="card" href="{href}"><h3>{escape(title)}</h3>'
                     f'<p>{summary}</p><span>詳細を見る →</span></a>')
    return ('<section aria-labelledby="pages"><h2 id="pages">ページ一覧</h2>'
            f'<div class="card-grid">{"".join(cards)}</div></section>')


def breadcrumbs(label, slug=None, title=None):
    section = (f'<a href="index.html">{label}</a>' if slug and slug != "index"
               else f'<span>{label}</span>')
    current = f'<li><span aria-current="location">{escape(title)}</span></li>' if title else ""
    return (f'<nav class="breadcrumbs" aria-label="パンくずリスト"><ol>'
            f'<li><a href="../index.html">トップ</a></li><li>{section}</li>{current}</ol></nav>')


def section_nav(label, pages, current):
    active = ' aria-current="page"'
    items = "".join(
        f'<li><a href="{slug}.html"{active if slug == current else ""}>'
        f'{escape(page[0])}</a></li>' for slug, page in pages.items()
    )
    nav = f'<nav class="section-pages" aria-label="{label} のページ"><ul>{items}</ul></nav>'
    return (f'<aside class="sidebar"><div class="desktop-only panel">'
            f'<a class="section-link" href="index.html#pages">{label} のページ一覧</a>{nav}</div>'
            f'<details class="mobile-only panel"><summary>{label} のページ一覧</summary>'
            f'<a class="section-link" href="index.html#pages">セクションのトップ</a>{nav}</details></aside>')


def pager(pages, current):
    slugs = list(pages)
    index = slugs.index(current)
    links = []
    for offset, label, rel in ((-1, "← 前へ", "prev"), (1, "次へ →", "next")):
        target = index + offset
        if 0 <= target < len(slugs):
            slug = slugs[target]
            links.append(f'<a href="{slug}.html" rel="{rel}"><span>{label}</span>'
                         f'<strong>{escape(pages[slug][0])}</strong></a>')
    return f'<nav class="pager" aria-label="前後のページ">{"".join(links)}</nav>'


def document(title, prefix, main, active, *, bread="", side=""):
    nav_items = (
        ("トップ", f"{prefix}index.html", "home"),
        ("TypeScript", f"{prefix}ts/index.html", "ts"),
        ("C++", f"{prefix}api/index.html", "api"),
        ("GitHub", GITHUB, ""),
    )
    selected = ' class="active"'
    nav_links = "".join(
        f'<a href="{href}"{selected if section == active else ""}>{label}</a>'
        for label, href, section in nav_items
    )
    nav = f'<nav class="site-links" aria-label="サイト">{nav_links}</nav>'
    return f"""<!doctype html>
<html lang="ja">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="theme-color" content="#f3efe7">
  <title>{escape(title)} · BedrockDynamicSDK</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link rel="stylesheet" href="{escape(FONTS)}">
  <link rel="stylesheet" href="{prefix}assets/style.css">
</head>
<body>
  <a class="skip" href="#main">本文へ移動</a>
  <header class="site-header"><div class="header-inner">
    <a class="brand" href="{prefix}index.html">BedrockDynamicSDK</a>
    <div class="desktop-only">{nav}</div>
    <details class="site-menu mobile-only"><summary>メニュー</summary>{nav}</details>
  </div></header>
  <div class="layout">
    {bread}
    <div class="{'with-sidebar' if side else 'standalone'}">
      {side}
      <main id="main" tabindex="-1">{main}</main>
    </div>
  </div>
  <footer class="site-footer"><strong>BedrockDynamicSDK</strong>
    <nav aria-label="フッター"><a href="{prefix}index.html#quickstart">クイックスタート</a>
      <a href="{GITHUB}">GitHub</a><a href="https://pexserver.com/">PEXserver</a></nav>
  </footer>
</body>
</html>
"""


def build(out):
    (out / "assets").mkdir(parents=True, exist_ok=True)
    copyfile(SITE / "style.css", out / "assets/style.css")
    home = (SITE / "home.html").read_text(encoding="utf-8")
    for marker, text, language, label in (
        ("cpp", CPP["index"][3], "cpp", "hello.cpp"),
        ("ts", TS_FIRST, "ts", "index.ts"),
        ("cmake", (SITE / "quickstart/CMakeLists.txt").read_text(encoding="utf-8"), "cmake", "CMakeLists.txt"),
        ("build", "./build-linux.sh", "sh", "ビルド"),
        ("configure", "cmake -S . -B build -A x64 -DSDK_DIR=C:/path/to/BedrockDynamicSDK\n"
         "cmake --build build --config Release --target Hello", "sh", "CMake コマンド"),
        ("manifest", '{\n  "name": "Hello",\n  "version": "1.0.0",\n  "apiVersion": "1.0.0",\n'
         '  "gameVersions": ["1.26.65.1"],\n  "entry": "index.ts"\n}', "json", "manifest.json"),
    ):
        home = home.replace(f"<!-- {marker} -->", code(text, language, label))
    (out / "index.html").write_text(document("ドキュメント", "", home, "home"), encoding="utf-8")

    for label, folder, pages in GROUPS:
        (out / folder).mkdir(exist_ok=True)
        start = '<a class="action" href="#sdk-host">C++ SDK とホスト →</a>' if folder == "api" else ""
        overview = (f'<section class="hero panel"><p class="eyebrow">API リファレンス</p>'
                    f'<h1>{label}</h1><p class="lead">{SECTION_SUMMARIES[folder]}</p>{start}</section>')
        cards = section_cards(folder, pages)
        if folder == "ts":
            (out / folder / "index.html").write_text(
                document(label, "../", overview + cards, folder, bread=breadcrumbs(label)), encoding="utf-8")
        for slug, (title, summary, signatures, example, note) in pages.items():
            language = "ts" if folder == "ts" else "cpp"
            signatures = ts_reference(signatures) if language == "ts" else cpp_reference(slug, signatures)
            heading = (f'<h2 id="sdk-host">{escape(title)}</h2>' if slug == "index"
                       else f'<p class="eyebrow">{label} リファレンス</p><h1>{escape(title)}</h1>')
            sub = "h3" if slug == "index" else "h2"
            body = f"""<article class="panel article">
{heading}
<section><{sub}>概要</{sub}><p>{summary}</p></section>
<section class="signatures"><{sub}>シグネチャ</{sub}>
<p class="scroll-hint">↔ 長いコードや表は横にスクロールできます</p>{signatures}</section>
<section><{sub}>最小例</{sub}>{code(example, language, "最小例")}</section>"""
            if note:
                body += f'\n<section class="notice"><{sub}>注意</{sub}><p>{note}</p></section>'
            body += "</article>" + pager(pages, slug)
            if slug == "index":
                body = overview + cards + body
            (out / folder / f"{slug}.html").write_text(
                document(title, "../", body, folder, bread=breadcrumbs(label, slug, title),
                         side=section_nav(label, pages, slug)), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=Path("_site"))
    args = parser.parse_args()
    build(args.out)


if __name__ == "__main__":
    main()
