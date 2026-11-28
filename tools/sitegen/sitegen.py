#!/usr/bin/env python3
"""Build the public quick reference with the standard library only."""

import argparse
from html import escape
from pathlib import Path
import re
from shutil import copyfile

from ts_pages import FIRST as TS_FIRST, PAGES as TS
from cpp_pages import PAGES as CPP

ROOT = Path(__file__).resolve().parents[2]
SITE = ROOT / "docs/site"
TYPES = ROOT / "sdk/ts/node_modules/@bedrock/sdk/index.d.ts"
GITHUB = "https://github.com/gamelist1990/BedrockDynamicSDK"
GROUPS = (("TypeScript", "ts", TS), ("C++", "api", CPP))

TOKEN = re.compile(
    r'//[^\n]*|"(?:\\.|[^"\\])*"|'
    r'\b(?:import|from|export|declare|const|let|if|return|true|false|null|undefined|'
    r'void|bool|boolean|number|string|class|struct|enum|namespace|using|public|'
    r'private|readonly|interface|type|static|virtual|override|noexcept|auto|'
    r'new|throw|unsigned|double|float|function|async|await|template|typename)\b'
)


def code(text, language):
    text = text.strip()
    parts, pos = [], 0
    for match in TOKEN.finditer(text):
        token = match[0]
        style = "comment" if token.startswith("//") else "string" if token.startswith('"') else "keyword"
        parts.extend((escape(text[pos:match.start()]), f'<span class="{style}">{escape(token)}</span>'))
        pos = match.end()
    parts.append(escape(text[pos:]))
    return f'<pre tabindex="0"><code class="language-{language}">{"".join(parts)}</code></pre>'


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


def catalogue(prefix="", current=""):
    def link(folder, slug, title):
        url = f"{folder}/{slug}"
        active = ' aria-current="page"' if current == url else ""
        return f'<li><a href="{prefix}{url}.html"{active}>{escape(title)}</a></li>'

    return "\n".join(
        f'<section><h2>{label}</h2><ul>' + "".join(
            link(folder, slug, page[0]) for slug, page in pages.items()
        ) + '</ul></section>'
        for label, folder, pages in GROUPS
    )


def document(title, prefix, main, current=""):
    side = (
        f'<nav class="sidebar" aria-label="API 一覧">{catalogue(prefix, current)}</nav>'
        if current else ""
    )
    return f"""<!doctype html>
<html lang="ja">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{escape(title)} · BedrockDynamicSDK</title>
  <link rel="stylesheet" href="{prefix}assets/style.css">
</head>
<body>
  <a class="skip" href="#main">本文へ移動</a>
  <header><nav aria-label="サイト"><a class="brand" href="{prefix}index.html">BedrockDynamicSDK</a>
    <a href="{prefix}index.html#apis">API 一覧</a><a href="{GITHUB}">GitHub</a></nav></header>
  <div class="layout{' with-sidebar' if side else ''}">
    <main id="main" tabindex="-1">{main}</main>
    {side}
  </div>
</body>
</html>
"""


def build(out):
    (out / "assets").mkdir(parents=True, exist_ok=True)
    copyfile(SITE / "style.css", out / "assets/style.css")
    home = (SITE / "home.html").read_text(encoding="utf-8")
    cpp_example = code(CPP["index"][3], "cpp")
    ts_example = code(TS_FIRST, "ts")
    cmake = code((SITE / "quickstart/CMakeLists.txt").read_text(encoding="utf-8"), "cmake")
    home = home.replace("<!-- cmake -->", cmake)
    home = home.replace("<!-- cpp -->", cpp_example).replace("<!-- ts -->", ts_example)
    home = home.replace("<!-- apis -->", catalogue())
    (out / "index.html").write_text(document("ドキュメント", "", home), encoding="utf-8")

    for label, folder, pages in GROUPS:
        (out / folder).mkdir(exist_ok=True)
        for slug, (title, summary, signatures, example, note) in pages.items():
            language = "ts" if folder == "ts" else "cpp"
            if language == "ts":
                signatures = ts_signatures(signatures)
            body = f"""<p class="eyebrow">{label}</p>
<h1>{escape(title)}</h1>
<section><h2>概要</h2><p>{summary}</p></section>
<section><h2>シグネチャ</h2>{code(signatures, language)}</section>
<section><h2>最小例</h2>{code(example, language)}</section>"""
            if note:
                body += f'\n<section><h2>注意</h2><p>{note}</p></section>'
            current = f"{folder}/{slug}"
            (out / f"{current}.html").write_text(document(title, "../", body, current), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=Path("_site"))
    args = parser.parse_args()
    build(args.out)


if __name__ == "__main__":
    main()
