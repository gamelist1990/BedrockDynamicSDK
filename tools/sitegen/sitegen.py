#!/usr/bin/env python3
"""Generate the BedrockDynamicSDK Pages site and XML indexes from source."""
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
    "content_log": "Write directly to Minecraft's Creator Content Log from a C++ mod.",
    "display": "Client-side brightness/gamma override that never writes player options.",
    "events": "Generic event signals and RAII subscriptions.",
    "game_types": "Vectors, positions, rotations, items, components and effects.",
    "keybinds": "Key binding registration, dispatch and change events.",
    "logging": "SDK log levels, tagged loggers, and optional file sinks.",
    "mod": "Mod base class: id, settings, system, world and keybinds.",
    "result": "Result<T> / Result<void> with typed error codes.",
    "settings": "Settings page, setting kinds and localization.",
    "system": "Tick-based task scheduling owned by one Mod.",
    "world": "World, dimensions, blocks, entities and players.",
}

LOGGING_MODULES = {
    "content_log": "Creator Content Log",
    "logging": "Logger and file sinks",
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


# This is a lightweight header scanner, not a C++ compiler. Ignore punctuation
# inside comments and ordinary literals when looking for declaration boundaries.
CPP_TOKEN_RE = re.compile(r'''//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[{}()[\];]''', re.DOTALL)
ACCESS_RE = re.compile(r"^(public|private|protected)\s*:\s*")
NON_FUNCTION_RE = re.compile(r"^(?:template\s*<[^>]*>\s*)?(?:class|struct|enum|namespace|using)\b")


def closing_brace(text: str, start: int) -> int:
    depth = 1
    for match in CPP_TOKEN_RE.finditer(text, start):
        token = match[0]
        if token == "{":
            depth += 1
        elif token == "}":
            depth -= 1
            if depth == 0:
                return match.start()
    return len(text)


def declaration_lines(body: str):
    """Split compact declarations without splitting inline bodies or parameters."""
    depth = 0
    for raw in body.splitlines():
        if raw.lstrip().startswith(("//", "#")):
            yield raw
            continue
        # Carry open delimiters across physical lines.
        start = 0
        end = len(raw)
        for match in CPP_TOKEN_RE.finditer(raw):
            token = match[0]
            if token.startswith("//"):
                end = match.start()
                break
            if token in ("{", "(", "["):
                depth += 1
            elif token in ("}", ")", "]"):
                depth -= 1
            elif token == ";" and depth == 0:
                yield raw[start:match.end()]
                start = match.end()
        if start == 0 or raw[start:end].strip():
            yield raw[start:end]


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


def parameter_end(decl: str, start: int) -> int:
    depth = 0
    for match in CPP_TOKEN_RE.finditer(decl, start):
        if match[0] == "(":
            depth += 1
        elif match[0] == ")":
            depth -= 1
            if depth == 0:
                return match.end()
    return -1


def is_function(decl: str) -> bool:
    start = decl.find("(")
    if start <= 0 or NON_FUNCTION_RE.match(decl):
        return False
    end = parameter_end(decl, start)
    if end == -1:
        return False
    if re.match(r"\s*[*&]", decl[start + 1:]) and decl[end:].lstrip().startswith("("):
        return False
    after = decl[end:].strip()
    return not re.match(r"^(?!(?:const|volatile|noexcept|override|final)\b)[A-Za-z_]\w*\s*[;=]", after)


def function_signature(decl: str) -> str:
    end = parameter_end(decl, decl.find("("))
    # Drop inline bodies and constructor initializer lists, not C++ scopes.
    after = re.split(r"(?<!:):(?!:)|[;{]", decl[end:], maxsplit=1)[0].strip()
    return decl[:end].strip() + (f" {after}" if after else "") + ";"


def parse_members(body: str, is_class: bool):
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
        if decl.startswith("friend "):
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
            # Compact nested definitions are a single declaration. Do not
            # print their entire bodies (which can include private members).
            sig = (decl.split("{", 1)[0].rstrip() + ";"
                   if NON_FUNCTION_RE.match(decl) and "{" in decl else decl)
            kind = "field"
        members.append({"kind": kind, "signature": sig, "doc": " ".join(current_doc).strip()})
        pending = ""
        current_doc = []

    for raw in declaration_lines(body):
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
        label = ACCESS_RE.match(stripped) if not pending else None
        if label:
            flush()
            access = label[1]
            stripped = stripped[label.end():]
            if not stripped:
                continue
        if stripped.startswith("#"):
            continue
        if pending.strip():
            pending += " " + stripped
        else:
            pending = stripped
        braces = [m[0] for m in CPP_TOKEN_RE.finditer(pending)]
        if braces.count("{") == braces.count("}") and (
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
SDK_NAMESPACE_RE = re.compile(r"^namespace\s+bedrock::sdk\s*\{", re.MULTILINE)


def parse_free_functions(text: str):
    functions = []
    for match in SDK_NAMESPACE_RE.finditer(text):
        body = text[match.end():closing_brace(text, match.end())]
        functions.extend(member for member in parse_members(body, False)
                         if member["kind"] == "function")
    return functions


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
        body = text[m.end():closing_brace(text, m.end())]
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
        summary = (MODULE_SUMMARIES[name] if name in LOGGING_MODULES
                   else module_doc or MODULE_SUMMARIES.get(name, ""))
        if not summary:
            summary = "Public C++ SDK module."
        modules.append(
            {
                "name": name,
                "path": rel,
                "doc": summary,
                "types": types,
                "functions": parse_free_functions(text),
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
        if mod["functions"]:
            functions_el = ET.SubElement(m_el, "functions")
            for fn in mod["functions"]:
                fn_el = ET.SubElement(functions_el, "function", {"signature": fn["signature"]})
                ET.SubElement(fn_el, "description").text = fn["doc"]
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

.api-layout { display: grid; grid-template-columns: 240px minmax(0, 1fr); gap: 32px; align-items: start; }
@media (max-width: 820px) {
  .api-layout { grid-template-columns: minmax(0, 1fr); }
  .api-layout .side-nav { position: static; max-height: 12rem; }
}
.side-nav {
  position: sticky; top: 80px; align-self: start;
  max-height: calc(100vh - 96px); overflow: auto;
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
.api-section { margin-bottom: 32px; scroll-margin-top: 96px; }
.related-api { margin: 0 0 24px; color: var(--muted); }

.type-block { margin-bottom: 32px; }
.type-block h2 { margin: 0 0 4px; }
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
      <a href="{prefix}api/index.html#logging">Logging</a>
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


def module_cards(modules):
    cards = []
    for mod in modules:
        count = len(mod["types"])
        mem_count = sum(len(t["members"]) for t in mod["types"])
        fn_count = len(mod["functions"])
        functions = f" &middot; {fn_count} free function{'s' if fn_count != 1 else ''}" if fn_count else ""
        url = f"{mod['name']}.html"
        search = f"{mod['name']} {mod['doc']}"
        label = LOGGING_MODULES.get(mod["name"], mod["name"])
        cards.append(f"""
      <article class="module-card" data-search="{esc(search)}">
        <h3><a href="{esc(url)}">{esc(label)}</a></h3>
        <div class="count">{count} types &middot; {mem_count} members{functions}</div>
        <p>{inline_code(mod['doc'])}</p>
      </article>""")
    return "".join(cards)


def render_api_index(out: Path, modules):
    api_dir = out / "api"
    api_dir.mkdir(parents=True, exist_ok=True)
    by_name = {mod["name"]: mod for mod in modules}
    logging_cards = module_cards(by_name[name] for name in LOGGING_MODULES if name in by_name)
    other_cards = module_cards(mod for mod in modules if mod["name"] not in LOGGING_MODULES)
    html_doc = page_head("API Reference", 1) + f"""
<main class="container">
  <section class="hero">
    <h1>API Reference</h1>
    <p>Modules are generated from <code>sdk/include/bedrock/sdk.hpp</code> and <code>sdk/include/bedrock/sdk/*.hpp</code>.</p>
  </section>
  <section class="api-section" id="logging">
    <h2>Logging &amp; Content Log</h2>
    <p>Choose <code>Logger</code> for ordinary SDK logging or
    <a href="content_log.html"><code>ContentLog::write</code></a> to write only to the game's Creator Content Log.</p>
    <div class="module-card-grid">{logging_cards}
    </div>
  </section>
  <section class="api-section">
    <h2>Other C++ modules</h2>
    <div class="module-card-grid">{other_cards}
    </div>
  </section>
</main>
<footer class="site-footer"><div class="container"><a href="../index.html">Home</a></div></footer>
<script src="../assets/app.js" defer></script>
</body>
</html>
"""
    (api_dir / "index.html").write_text(html_doc, encoding="utf-8")


def signature_table(entries):
    if not entries:
        return ""
    rows = "".join(f"""
        <tr>
          <td><code>{esc(entry['signature'])}</code></td>
          <td><div class="doc">{inline_code(entry['doc'])}</div></td>
        </tr>""" for entry in entries)
    return f"""
      <table>
        <thead><tr><th scope="col">Signature</th><th scope="col">Description</th></tr></thead>
        <tbody>{rows}
        </tbody>
      </table>"""


def logging_module_intro(name):
    if name == "logging":
        return """
      <p class="related-api">For direct writes that bypass SDK sinks, see <a href="content_log.html">Creator Content Log</a>.</p>"""
    if name != "content_log":
        return ""
    example = """#include <bedrock/sdk.hpp>

void announceReady() {
    using namespace bedrock::sdk;
    if (ContentLog::available()) {
        ContentLog::write(LogLevel::Info, "MyMod", "Ready");
    }
}"""
    return f"""
      <section class="api-section">
        <h2>Write from a C++ mod</h2>
        <p>Link <code>bedrock::sdk</code> and include <code>&lt;bedrock/sdk.hpp&gt;</code>.
        Call from <code>Mod::onEnable()</code> or later when using <code>ClientHost</code>.
        If the route is unavailable, writes are safe no-ops.</p>
        <pre><code>{esc(example)}</code></pre>
        <p>Find the game’s Content Log options in Settings → Creator. Availability only
        means the SDK writer is installed: the game may still suppress the line, and
        the API does not confirm whether the line was displayed or saved.</p>
        <p class="related-api">For ordinary logging and SDK-owned files, see <a href="logging.html">Logger and file sinks</a>.</p>
      </section>"""


def render_module_page(out: Path, modules, current):
    api_dir = out / "api"
    nav_links = ""
    for mod in modules:
        active = ' class="active" aria-current="page"' if mod["name"] == current["name"] else ""
        label = LOGGING_MODULES.get(mod["name"], mod["name"])
        nav_links += f'<a{active} href="{esc(mod["name"])}.html">{esc(label)}</a>\n'
    reference = ""
    for t in current["types"]:
        bases_html = f'<div class="bases">: {esc(t["bases"])}</div>' if t["bases"] else ""
        doc_html = f'<div class="doc">{inline_code(t["doc"])}</div>' if t["doc"] else ""
        reference += f"""
    <div class="type-block">
      <div class="kind">{esc(t['kind'])}</div>
      <h2>{esc(t['name'])}</h2>
      {bases_html}
      {doc_html}
      {signature_table(t['members'])}
    </div>"""
    if current["functions"]:
        reference += f"""
    <section class="type-block">
      <h2>Free functions</h2>
      {signature_table(current['functions'])}
    </section>"""

    label = LOGGING_MODULES.get(current["name"], current["name"])
    html_doc = page_head(f"API: {label}", 1) + f"""
<main class="container">
  <div class="api-layout">
    <aside class="side-nav">
      <h3>Modules</h3>
{nav_links}    </aside>
    <div>
      <section class="hero" style="padding-top:0">
        <h1>{esc(label)}</h1>
        <p>{inline_code(current['doc'])}</p>
      </section>
{logging_module_intro(current['name'])}
{reference}
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
