#!/usr/bin/env python3
"""Generate the BedrockDynamicSDK Pages site from XML."""
from __future__ import annotations

import argparse
import html
import re
import shutil
import xml.etree.ElementTree as ET
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SDK_INCLUDE = REPO_ROOT / "sdk" / "include"
DOCS_DIR = REPO_ROOT / "docs"

CATEGORIES = [
    ("api", "API ガイド"),
    ("maintenance", "保守・更新"),
    ("design", "設計・構造"),
    ("reverse", "逆解析・調査ログ"),
    ("build", "ビルド・開発環境"),
]

CATEGORY_MAP = {
    "client-api-quickstart.md": "api",
    "command-requests.md": "api",
    "native-client.md": "api",
    "native-client-commands.md": "api",
    "packet-events.md": "api",
    "typed-sdk-api.md": "api",
    "world-block-camera-api.md": "api",
    "world-events-and-packets.md": "api",
    "camera-game-type-brightness.md": "api",
    "profile-maintenance.md": "maintenance",
    "version-update-workflow.md": "maintenance",
    "signature-porting.md": "maintenance",
    "checksig-jobs.md": "maintenance",
    "checksig-auto-dev.md": "maintenance",
    "checksig-126605.md": "maintenance",
    "data-format.md": "maintenance",
    "native-profile-layout.md": "maintenance",
    "resource-packs.md": "maintenance",
    "sdk-structure.md": "design",
    "stage-a-sdk-design.md": "design",
    "shared-hook-runtime.md": "design",
    "freecam-native.md": "reverse",
    "hitbox-world-render.md": "reverse",
    "speedbuilder-native.md": "reverse",
    "top-native.md": "reverse",
    "native-keyboard-input.md": "reverse",
    "view-actor-enumeration-investigation.md": "reverse",
    "actor-unique-id-static-analysis.md": "reverse",
    "linux-windows-x64-build.md": "build",
}

MODULE_SUMMARIES = {
    "sdk": "Aggregate header including every public SDK module.",
    "client_host": "Client host lifecycle, multi-Mod attach/detach, and display overrides.",
    "display": "Client-side brightness/gamma override that never writes player options.",
    "events": "Generic event signals and RAII subscriptions.",
    "game_types": "Vectors, positions, rotations, items, components and effects.",
    "keybinds": "Key binding registration, dispatch and change events.",
    "mod": "Mod base class: id, settings, system, world and keybinds.",
    "result": "Result<T> / Result<void> with typed error codes.",
    "settings": "Settings page, setting kinds and localization.",
    "system": "Tick-based task scheduling owned by one Mod.",
    "world": "World, dimensions, blocks, entities and players.",
}

GITHUB_REPO = "gamelist1990/BedrockDynamicSDK"
GITHUB_BASE = f"https://github.com/{GITHUB_REPO}"


def strip_slash(line: str) -> str:
    s = line.strip()
    if s.startswith("///"):
        return s[3:].strip()
    if s.startswith("//"):
        return s[2:].strip()
    return s


def count_braces(s: str) -> int:
    depth = 0
    in_str = False
    i = 0
    while i < len(s):
        c = s[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_str = False
        else:
            if c == '"':
                in_str = True
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
        i += 1
    return depth


def parse_enum_members(body: str):
    segments = []
    depth = 0
    cur = ""
    for ch in body:
        if ch == "{":
            depth += 1
            cur += ch
        elif ch == "}":
            depth -= 1
            cur += ch
        elif ch == "," and depth == 0:
            segments.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        segments.append(cur)
    members = []
    for seg in segments:
        seg = seg.strip()
        seg = seg.split("//")[0].strip()
        if not seg:
            continue
        members.append({"kind": "enum-constant", "signature": seg, "doc": ""})
    return members


def is_function(decl: str) -> bool:
    start = decl.find("(")
    if start == -1:
        return False
    prefix = decl[:start].rstrip()
    if not prefix:
        return False
    depth = 0
    i = start
    while i < len(decl):
        if decl[i] == "(":
            depth += 1
        elif decl[i] == ")":
            depth -= 1
            if depth == 0:
                break
        i += 1
    if i >= len(decl):
        return False
    after = decl[i + 1:].strip()
    if re.match(r"^[A-Za-z_]\w*\s*[;=]", after):
        return False
    return True


def function_signature(decl: str) -> str:
    start = decl.find("(")
    if start == -1:
        base = decl.split("{", 1)[0].strip()
        return base if base.endswith(";") else base + ";"
    depth = 0
    i = start
    while i < len(decl):
        if decl[i] == "(":
            depth += 1
        elif decl[i] == ")":
            depth -= 1
            if depth == 0:
                break
        i += 1
    if i >= len(decl):
        base = decl.split("{", 1)[0].strip()
        return base if base.endswith(";") else base + ";"
    after = decl[i + 1:]
    q = ""
    for ch in after:
        if ch in ":;{":
            break
        q += ch
    q = q.strip()
    base = decl[: i + 1].strip()
    sig = base + ((" " + q) if q else "")
    if not sig.endswith(";") and not sig.endswith("}") and not sig.endswith("{"):
        sig += ";"
    return sig


def parse_members(body: str, is_class: bool):
    lines = body.splitlines()
    access = "private" if is_class else "public"
    current_doc = []
    pending = ""
    members = []

    def flush():
        nonlocal pending, current_doc
        decl = " ".join(pending.split())
        if not decl:
            pending = ""
            current_doc = []
            return
        if pending.strip() == "":
            pending = ""
            current_doc = []
            return
        if "friend " in decl:
            pending = ""
            current_doc = []
            return
        if decl.startswith("using ") or decl.startswith("static_assert") or decl.startswith("#"):
            pending = ""
            current_doc = []
            return
        if access != "public":
            pending = ""
            current_doc = []
            return
        if is_function(decl):
            sig = function_signature(decl)
            kind = "function"
        else:
            sig = decl
            kind = "field"
        members.append({"kind": kind, "signature": sig, "doc": " ".join(current_doc).strip()})
        pending = ""
        current_doc = []

    for raw in lines:
        stripped = raw.strip()
        if not stripped:
            if pending.strip():
                pending += " "
            else:
                current_doc = []
            continue
        if stripped.startswith("//"):
            if not pending.strip():
                current_doc.append(strip_slash(stripped))
            else:
                # comment after code; ignore for signature
                pass
            continue
        if stripped in ("public:", "private:", "protected:"):
            flush()
            access = stripped[:-1]
            current_doc = []
            continue
        if stripped.startswith("#"):
            continue
        if pending.strip():
            pending += " " + stripped
        else:
            pending = stripped
        if count_braces(pending) == 0 and (
            pending.rstrip().endswith(";")
            or pending.rstrip().endswith("}")
            or "= default" in pending
            or "= delete" in pending
        ):
            flush()
    if pending.strip():
        flush()
    return members


TYPE_RE = re.compile(
    r"^(?:template\s*<[^>]*>\s*\n\s*)?(class|struct|enum)\s+(?:class\s+)?(\w+(?:<[^>]*>)?)(?:\s+final)?(?:\s*:\s*([^\{]+?))?\s*\{",
    re.MULTILINE,
)


def parse_header(text: str):
    types = []
    lines = text.splitlines()
    for m in TYPE_RE.finditer(text):
        # ensure match at start of line (no leading whitespace) -> top-level
        line_start = text.rfind("\n", 0, m.start()) + 1
        if text[line_start:m.start()].strip() != "":
            continue
        kind = m.group(1)
        name = m.group(2)
        bases = (m.group(3) or "").strip().rstrip(",")
        # doc comments preceding
        start_line = text[:m.start()].count("\n")
        doc_lines = []
        j = start_line - 1
        while j >= 0 and lines[j].strip().startswith("//"):
            doc_lines.insert(0, strip_slash(lines[j].strip()))
            j -= 1
        type_doc = " ".join(doc_lines).strip()
        # body
        depth = 1
        k = m.end()
        while k < len(text) and depth > 0:
            c = text[k]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
            k += 1
        body = text[m.end(): k - 1]
        if kind == "enum":
            members = parse_enum_members(body)
        else:
            members = parse_members(body, kind == "class")
        types.append(
            {
                "kind": kind,
                "name": name,
                "bases": bases,
                "doc": type_doc,
                "members": members,
            }
        )
    # module doc: first comment block before first type, fallback
    module_doc = ""
    # find first top-level type position
    first_type_pos = None
    for m in TYPE_RE.finditer(text):
        line_start = text.rfind("\n", 0, m.start()) + 1
        if text[line_start:m.start()].strip() == "":
            first_type_pos = m.start()
            break
    if first_type_pos is not None:
        prefix = text[:first_type_pos]
        # last comment block
        doc_lines = []
        for line in prefix.splitlines()[::-1]:
            if line.strip().startswith("//"):
                doc_lines.insert(0, strip_slash(line.strip()))
            elif line.strip() == "":
                if doc_lines:
                    break
            else:
                if doc_lines:
                    break
        module_doc = " ".join(doc_lines).strip()
    return module_doc, types


def collect_modules():
    headers = []
    if (SDK_INCLUDE / "bedrock" / "sdk.hpp").exists():
        headers.append(SDK_INCLUDE / "bedrock" / "sdk.hpp")
    sdk_dir = SDK_INCLUDE / "bedrock" / "sdk"
    if sdk_dir.exists():
        for p in sorted(sdk_dir.glob("*.hpp")):
            headers.append(p)
    # dedup
    seen = set()
    uniq = []
    for p in headers:
        if p in seen:
            continue
        seen.add(p)
        uniq.append(p)
    modules = []
    for p in uniq:
        text = p.read_text(encoding="utf-8")
        module_doc, types = parse_header(text)
        rel = p.relative_to(REPO_ROOT).as_posix()
        name = p.stem
        summary = module_doc or MODULE_SUMMARIES.get(name, "")
        if not summary:
            summary = "Public C++ SDK module."
        modules.append(
            {
                "name": name,
                "path": rel,
                "doc": summary,
                "types": types,
            }
        )
    return modules


def collect_docs():
    articles = []
    if not DOCS_DIR.exists():
        return articles
    for md in sorted(DOCS_DIR.glob("*.md")):
        text = md.read_text(encoding="utf-8")
        title = ""
        for line in text.splitlines():
            if line.startswith("# "):
                title = line[2:].strip()
                break
        if not title:
            title = md.stem
        # first non-empty paragraph
        summary = ""
        lines = text.splitlines()
        para = []
        in_para = False
        for line in lines:
            s = line.strip()
            if not s:
                if in_para:
                    break
                continue
            if s.startswith("#"):
                if in_para:
                    break
                continue
            if s.startswith("```"):
                if in_para:
                    break
                continue
            in_para = True
            para.append(s)
        summary = " ".join(para).strip()
        if len(summary) > 200:
            summary = summary[:197].rstrip() + "..."
        cat = CATEGORY_MAP.get(md.name, "design")
        if cat not in {c[0] for c in CATEGORIES}:
            cat = "design"
        articles.append(
            {
                "id": md.stem,
                "title": title,
                "category": cat,
                "file": f"docs/{md.name}",
                "summary": summary,
            }
        )
    return articles


def esc(s: str) -> str:
    return html.escape(s or "", quote=True)


def inline_code(s: str) -> str:
    # convert backticks to <code>
    s = esc(s)
    s = re.sub(r"`([^`]+)`", r"<code>\1</code>", s)
    return s


def write_xml(out: Path, modules, articles):
    # api.xml
    api_root = ET.Element("api")
    mods_el = ET.SubElement(api_root, "modules")
    for mod in modules:
        m_el = ET.SubElement(mods_el, "module", {"name": mod["name"], "path": mod["path"]})
        doc_el = ET.SubElement(m_el, "doc")
        doc_el.text = mod["doc"]
        types_el = ET.SubElement(m_el, "types")
        for t in mod["types"]:
            t_el = ET.SubElement(
                types_el,
                "type",
                {
                    "kind": t["kind"],
                    "name": t["name"],
                },
            )
            if t["bases"]:
                t_el.set("bases", t["bases"])
            d = ET.SubElement(t_el, "doc")
            d.text = t["doc"]
            mems_el = ET.SubElement(t_el, "members")
            for mem in t["members"]:
                mem_el = ET.SubElement(
                    mems_el,
                    "member",
                    {
                        "kind": mem["kind"],
                        "signature": mem["signature"],
                    },
                )
                desc = ET.SubElement(mem_el, "description")
                desc.text = mem["doc"]
    ET.indent(api_root, space="  ")
    (out / "api.xml").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(api_root, encoding="unicode") + "\n",
        encoding="utf-8",
    )

    # docs.xml
    docs_root = ET.Element("docs")
    cats_el = ET.SubElement(docs_root, "categories")
    for cid, label in CATEGORIES:
        ET.SubElement(cats_el, "category", {"id": cid, "label": label})
    arts_el = ET.SubElement(docs_root, "articles")
    for art in articles:
        a_el = ET.SubElement(
            arts_el,
            "article",
            {"id": art["id"], "title": art["title"], "category": art["category"], "file": art["file"]},
        )
        s_el = ET.SubElement(a_el, "summary")
        s_el.text = art["summary"]
    ET.indent(docs_root, space="  ")
    (out / "docs.xml").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(docs_root, encoding="unicode") + "\n",
        encoding="utf-8",
    )


CSS = r"""
:root {
  --bg: #faf6ed;
  --ink: #171717;
  --ink-soft: #25282e;
  --muted: #6b655e;
  --line: #dcdacf;
  --accent: #a7472b;
  --accent-2: #c75b39;
  --blue: #0f5d79;
  --panel: #fffcf6;
  --code: #f1ece2;
  --radius: 12px;
  --shadow: 0 12px 32px rgba(48, 41, 28, 0.08);
}
* { box-sizing: border-box; }
html { scroll-behavior: smooth; }
body {
  margin: 0;
  font-family: "Noto Sans JP", "Hiragino Sans", "Yu Gothic", system-ui, -apple-system, "Segoe UI", sans-serif;
  color: var(--ink);
  background: var(--bg);
  line-height: 1.7;
}
h1, h2, h3, h4 {
  font-family: "Space Grotesk", "Noto Sans JP", sans-serif;
  color: var(--ink-soft);
  letter-spacing: -0.02em;
  line-height: 1.25;
  text-wrap: balance;
}
p { text-wrap: pretty; }
a { color: var(--accent); text-decoration: none; }
a:hover { color: var(--accent-2); text-decoration: underline; }
code, pre { font-family: "Space Grotesk", "Consolas", monospace; }

.container { width: min(1180px, calc(100% - 48px)); margin: 0 auto; }

.site-header {
  position: sticky; top: 0; z-index: 20;
  background: rgba(250, 246, 237, 0.92);
  backdrop-filter: blur(8px);
  border-bottom: 1px solid var(--line);
}
.nav-inner {
  display: flex; align-items: center; justify-content: space-between;
  padding: 14px 0; gap: 16px; flex-wrap: wrap;
}
.brand { font-family: "Space Grotesk", sans-serif; font-weight: 700; font-size: 1.05rem; color: var(--ink-soft); }
.brand span { color: var(--accent); }
.nav-links { display: flex; gap: 18px; flex-wrap: wrap; }
.nav-links a { color: var(--ink-soft); font-size: 0.95rem; }
.nav-links a:hover { color: var(--accent); text-decoration: none; }

.hero { padding: 56px 0 24px; }
.hero h1 { font-size: clamp(2rem, 4vw, 3rem); margin: 0 0 12px; }
.hero p { color: var(--muted); max-width: 62ch; margin: 0 0 20px; }
.search {
  width: 100%; max-width: 560px; padding: 12px 16px;
  border: 1px solid var(--line); border-radius: var(--radius);
  background: var(--panel); font: inherit; color: var(--ink);
}
.search:focus { outline: 2px solid var(--accent); outline-offset: 2px; }

.downloads {
  margin: 24px 0 40px; padding: 18px 20px;
  border: 1px solid var(--line); border-radius: var(--radius);
  background: var(--code); display: flex; align-items: center;
  justify-content: space-between; gap: 16px; flex-wrap: wrap;
}
.downloads strong { color: var(--ink-soft); }
.downloads span { color: var(--muted); font-size: 0.9rem; }
.button {
  display: inline-block; padding: 10px 16px; border-radius: var(--radius);
  background: var(--accent); color: #faf6ed; font-weight: 600; font-size: 0.9rem;
}
.button:hover { background: var(--accent-2); color: #faf6ed; text-decoration: none; }

section.category { margin-bottom: 40px; }
section.category h2 {
  font-size: 1.35rem; border-bottom: 1px solid var(--line);
  padding-bottom: 8px; margin: 0 0 16px;
}
.cards { display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: 16px; }
.card {
  background: var(--panel); border: 1px solid var(--line); border-radius: var(--radius);
  padding: 18px; box-shadow: var(--shadow);
}
.card h3 { margin: 0 0 6px; font-size: 1.05rem; }
.card p { margin: 0; color: var(--muted); font-size: 0.9rem; }
.card .meta { margin-top: 8px; color: var(--muted); font-size: 0.78rem; }

.api-layout { display: grid; grid-template-columns: 240px 1fr; gap: 32px; align-items: start; }
@media (max-width: 820px) { .api-layout { grid-template-columns: 1fr; } }
.side-nav {
  position: sticky; top: 80px; align-self: start;
  border: 1px solid var(--line); border-radius: var(--radius);
  background: var(--panel); padding: 12px;
}
.side-nav h3 { margin: 0 0 8px; font-size: 0.85rem; color: var(--muted); text-transform: uppercase; letter-spacing: 0.05em; }
.side-nav a { display: block; padding: 6px 10px; border-radius: 8px; color: var(--ink-soft); font-size: 0.9rem; }
.side-nav a:hover { background: var(--code); color: var(--accent); text-decoration: none; }
.side-nav a.active { background: var(--code); color: var(--accent); font-weight: 600; }

.module-card-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); gap: 16px; }
.module-card {
  background: var(--panel); border: 1px solid var(--line); border-radius: var(--radius);
  padding: 18px; box-shadow: var(--shadow);
}
.module-card h3 { margin: 0 0 4px; font-size: 1.05rem; }
.module-card .count { color: var(--muted); font-size: 0.8rem; margin-bottom: 6px; }
.module-card p { margin: 0; color: var(--muted); font-size: 0.88rem; }

.type-block { margin-bottom: 32px; }
.type-block h3 { margin: 0 0 4px; }
.type-block .kind { color: var(--muted); font-size: 0.8rem; text-transform: uppercase; letter-spacing: 0.05em; }
.type-block .bases { color: var(--blue); font-size: 0.9rem; margin-bottom: 6px; }
.type-block .doc { color: var(--ink-soft); margin: 6px 0 12px; }
table { width: 100%; border-collapse: collapse; }
th, td { padding: 10px 12px; border-bottom: 1px solid var(--line); vertical-align: top; text-align: left; }
th { font-family: "Space Grotesk", sans-serif; font-size: 0.8rem; color: var(--muted); text-transform: uppercase; letter-spacing: 0.05em; font-weight: 600; }
td code { background: var(--code); padding: 2px 6px; border-radius: 6px; font-size: 0.85rem; white-space: pre-wrap; word-break: break-word; }
td .doc { color: var(--muted); font-size: 0.88rem; }

pre, .code-panel {
  background: var(--code); border: 1px solid var(--line); border-radius: var(--radius);
  padding: 16px; overflow: auto; font-size: 0.88rem;
}

footer.site-footer {
  margin-top: 48px; padding: 24px 0; border-top: 1px solid var(--line);
  color: var(--muted); font-size: 0.85rem;
}

.hidden { display: none; }
"""


APP_JS = r"""
(function () {
  const input = document.getElementById('search');
  if (!input) return;
  input.addEventListener('input', function (e) {
    const q = (e.target.value || '').trim().toLowerCase();
    document.querySelectorAll('.card, .module-card, .article-card').forEach(function (c) {
      const hay = (c.getAttribute('data-search') || c.textContent || '').toLowerCase();
      c.classList.toggle('hidden', !!q && hay.indexOf(q) === -1);
    });
    document.querySelectorAll('section.category').forEach(function (s) {
      const visible = s.querySelector('.card:not(.hidden), .module-card:not(.hidden), .article-card:not(.hidden)');
      s.classList.toggle('hidden', !!q && !visible);
    });
  });
})();
"""


def page_head(title: str, depth: int = 0) -> str:
    prefix = "" if depth == 0 else "../" * depth
    return f"""<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{esc(title)}</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@400;500;700&family=Noto+Sans+JP:wght@400;500;700&display=swap" rel="stylesheet">
<link rel="stylesheet" href="{prefix}assets/style.css">
</head>
<body>
<header class="site-header">
  <div class="container nav-inner">
    <a class="brand" href="{prefix}index.html">Bedrock<span>DynamicSDK</span></a>
    <nav class="nav-links">
      <a href="{prefix}api/index.html">API</a>
      <a href="{GITHUB_BASE}">GitHub</a>
      <a href="{GITHUB_BASE}/releases">Releases</a>
    </nav>
  </div>
</header>
"""


def render_index(out: Path, modules, articles):
    # manifest version
    version = ""
    manifest_path = REPO_ROOT / "data" / "manifest.json"
    if manifest_path.exists():
        try:
            import json
            version = json.loads(manifest_path.read_text(encoding="utf-8")).get("default_version", "")
        except Exception:
            version = ""
    if version:
        release_url = f"{GITHUB_BASE}/releases/tag/v{version}"
        downloads_html = f"""
  <div class="downloads">
    <div><strong>Latest release</strong><br><span>{esc(version)}</span></div>
    <a class="button" href="{release_url}">Release notes / download</a>
  </div>"""
    else:
        release_url = f"{GITHUB_BASE}/releases/latest"
        downloads_html = f"""
  <div class="downloads">
    <div><strong>Latest release</strong><br><span>see GitHub Releases</span></div>
    <a class="button" href="{release_url}">Release notes / download</a>
  </div>"""

    cat_map = {cid: label for cid, label in CATEGORIES}
    by_cat = {cid: [] for cid, _ in CATEGORIES}
    for art in articles:
        by_cat.setdefault(art["category"], []).append(art)

    sections_html = ""
    for cid, label in CATEGORIES:
        arts = by_cat.get(cid, [])
        if not arts:
            continue
        cards = ""
        for art in arts:
            url = f"{GITHUB_BASE}/blob/main/{art['file']}"
            search = f"{art['title']} {art['summary']}"
            cards += f"""
      <article class="card" data-search="{esc(search)}">
        <h3><a href="{esc(url)}" target="_blank" rel="noopener">{esc(art['title'])}</a></h3>
        <p>{inline_code(art['summary'])}</p>
      </article>"""
        sections_html += f"""
  <section class="category">
    <h2>{esc(label)}</h2>
    <div class="cards">{cards}
    </div>
  </section>"""

    html_doc = page_head("BedrockDynamicSDK", 0) + f"""
<main class="container">
  <section class="hero">
    <h1>BedrockDynamicSDK</h1>
    <p>Public C++20 SDK for Minecraft Bedrock native mods. API reference is generated from the headers; documentation index is generated from <code>docs/*.md</code>.</p>
    <input id="search" class="search" type="search" placeholder="Search docs, titles, summaries..." autocomplete="off">
  </section>
{downloads_html}
{sections_html}
</main>
<footer class="site-footer"><div class="container">Generated by tools/sitegen/sitegen.py. Source: <a href="{GITHUB_BASE}">GitHub</a>.</div></footer>
<script src="assets/app.js" defer></script>
</body>
</html>
"""
    (out / "index.html").write_text(html_doc, encoding="utf-8")


def render_api_index(out: Path, modules):
    api_dir = out / "api"
    api_dir.mkdir(parents=True, exist_ok=True)
    cards = ""
    for mod in modules:
        count = len(mod["types"])
        mem_count = sum(len(t["members"]) for t in mod["types"])
        url = f"{mod['name']}.html"
        search = f"{mod['name']} {mod['doc']}"
        cards += f"""
      <article class="module-card" data-search="{esc(search)}">
        <h3><a href="{esc(url)}">{esc(mod['name'])}</a></h3>
        <div class="count">{count} types &middot; {mem_count} members</div>
        <p>{inline_code(mod['doc'])}</p>
      </article>"""
    html_doc = page_head("API Reference", 1) + f"""
<main class="container">
  <section class="hero">
    <h1>API Reference</h1>
    <p>Modules are generated from <code>sdk/include/bedrock/sdk.hpp</code> and <code>sdk/include/bedrock/sdk/*.hpp</code>.</p>
  </section>
  <div class="module-card-grid">{cards}
  </div>
</main>
<footer class="site-footer"><div class="container"><a href="../index.html">Home</a></div></footer>
<script src="../assets/app.js" defer></script>
</body>
</html>
"""
    (api_dir / "index.html").write_text(html_doc, encoding="utf-8")


def render_module_page(out: Path, modules, current):
    api_dir = out / "api"
    # side nav
    nav_links = ""
    for mod in modules:
        active = "active" if mod["name"] == current["name"] else ""
        nav_links += f'<a class="{active}" href="{esc(mod["name"])}.html">{esc(mod["name"])}</a>\n'
    # types
    types_html = ""
    for t in current["types"]:
        kind = t["kind"]
        name = t["name"]
        bases = t["bases"]
        doc = t["doc"]
        bases_html = f'<div class="bases">: {esc(bases)}</div>' if bases else ""
        doc_html = f'<div class="doc">{inline_code(doc)}</div>' if doc else ""
        rows = ""
        for mem in t["members"]:
            sig = mem["signature"]
            desc = mem["doc"]
            rows += f"""
        <tr>
          <td><code>{esc(sig)}</code></td>
          <td><div class="doc">{inline_code(desc) if desc else ''}</div></td>
        </tr>"""
        table = ""
        if rows:
            table = f"""
      <table>
        <thead><tr><th>Signature</th><th>Description</th></tr></thead>
        <tbody>{rows}
        </tbody>
      </table>"""
        types_html += f"""
    <div class="type-block">
      <div class="kind">{esc(kind)}</div>
      <h3>{esc(name)}</h3>
      {bases_html}
      {doc_html}
      {table}
    </div>"""

    html_doc = page_head(f"API: {current['name']}", 1) + f"""
<main class="container">
  <div class="api-layout">
    <aside class="side-nav">
      <h3>Modules</h3>
{nav_links}    </aside>
    <div>
      <section class="hero" style="padding-top:0">
        <h1>{esc(current['name'])}</h1>
        <p>{inline_code(current['doc'])}</p>
      </section>
{types_html}
    </div>
  </div>
</main>
<footer class="site-footer"><div class="container"><a href="index.html">API index</a> &middot; <a href="../index.html">Home</a></div></footer>
<script src="../assets/app.js" defer></script>
</body>
</html>
"""
    (api_dir / f"{current['name']}.html").write_text(html_doc, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default="_site")
    args = parser.parse_args()
    out = Path(args.out)
    if not out.is_absolute():
        out = Path.cwd() / out
    out.mkdir(parents=True, exist_ok=True)
    (out / "assets").mkdir(parents=True, exist_ok=True)
    (out / "api").mkdir(parents=True, exist_ok=True)

    modules = collect_modules()
    articles = collect_docs()

    write_xml(out, modules, articles)

    (out / "assets" / "style.css").write_text(CSS.strip() + "\n", encoding="utf-8")
    (out / "assets" / "app.js").write_text(APP_JS.strip() + "\n", encoding="utf-8")

    render_index(out, modules, articles)
    render_api_index(out, modules)
    for mod in modules:
        render_module_page(out, modules, mod)

    print(f"Generated site in {out}")
    print(f"  modules: {len(modules)}")
    print(f"  articles: {len(articles)}")


if __name__ == "__main__":
    main()
