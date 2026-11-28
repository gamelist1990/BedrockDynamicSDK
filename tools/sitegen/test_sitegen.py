import re
import tempfile
import unittest
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit

import sitegen


class Page(HTMLParser):
    def __init__(self, path):
        super().__init__(convert_charrefs=True)
        self.ids = []
        self.links = []
        self.tags = []
        self.current = []
        self.headings = []
        self.in_main = False
        self.heading = None
        self.feed(path.read_text(encoding="utf-8"))

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        self.tags.append(tag)
        if "id" in attrs:
            self.ids.append(attrs["id"])
        if "href" in attrs:
            self.links.append(attrs["href"])
        if attrs.get("aria-current") == "page":
            self.current.append(attrs["href"])
        if tag == "main":
            self.in_main = True
        if self.in_main and tag == "h2":
            self.heading = []

    def handle_endtag(self, tag):
        if tag == "main":
            self.in_main = False
        if tag == "h2" and self.heading is not None:
            self.headings.append("".join(self.heading).strip())
            self.heading = None

    def handle_data(self, data):
        if self.heading is not None:
            self.heading.append(data)


class SiteTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        preview = tempfile.TemporaryDirectory()
        cls.addClassCleanup(preview.cleanup)
        cls.out = Path(preview.name) / "BedrockDynamicSDK"
        sitegen.build(cls.out)
        cls.pages = {path: Page(path) for path in cls.out.rglob("*.html")}

    def test_every_api_is_linked_and_has_only_the_required_sections(self):
        home = self.out / "index.html"
        expected = {home}
        for _, folder, pages in sitegen.GROUPS:
            for slug, (_, _, _, _, note) in pages.items():
                url = f"{folder}/{slug}.html"
                with self.subTest(url=url):
                    path = self.out / url
                    expected.add(path)
                    self.assertIn(url, self.pages[home].links)
                    self.assertEqual(self.pages[path].current, [f"../{url}"])
                    self.assertEqual(self.pages[path].headings,
                                     ["概要", "シグネチャ", "最小例"] + (["注意"] if note else []))
        self.assertEqual(set(self.pages), expected)

    def test_internal_links_and_fragments(self):
        for path, page in self.pages.items():
            self.assertEqual(len(page.ids), len(set(page.ids)), path)
            for link in page.links:
                with self.subTest(page=path.relative_to(self.out), link=link):
                    parts = urlsplit(link)
                    if parts.scheme or parts.netloc:
                        continue
                    relative = unquote(parts.path)
                    self.assertFalse(relative.startswith("/"), "GitHub project Pages uses a subpath")
                    target = (path.parent / relative).resolve() if relative else path
                    self.assertTrue(target.is_relative_to(self.out))
                    self.assertTrue(target.is_file(), target)
                    if parts.fragment:
                        self.assertIn(unquote(parts.fragment), self.pages[target].ids)

    def test_output_is_static_and_safe_html(self):
        for path, page in self.pages.items():
            self.assertNotIn("script", page.tags, path)
            self.assertEqual(page.tags.count("h1"), 1, path)
            self.assertIn("style.css", " ".join(page.links), path)
        self.assertNotIn("<x>", sitegen.code("const a: Vector<x> = value;", "ts"))
        self.assertIn("&lt;x&gt;", sitegen.code("const a: Vector<x> = value;", "ts"))

    def test_typescript_overloads_and_value_type_pairs_are_preserved(self):
        entity = sitegen.ts_signatures("Entity:getComponent")
        self.assertIn("getComponent<K extends keyof EntityComponentMap>", entity)
        self.assertIn("getComponent(id: string)", entity)
        slot = sitegen.ts_signatures("EquipmentSlot")
        self.assertIn("export declare const EquipmentSlot:", slot)
        self.assertIn("export type EquipmentSlot =", slot)
        with self.assertRaises(ValueError):
            sitegen.ts_signatures("System:unknownMember")

    def test_cpp_declarations_match_public_headers(self):
        # Compare the displayed declaration to its original declaration or
        # inline definition, ignoring layout, comments and [[nodiscard]].
        def compact(source):
            source = re.sub(r"//[^\n]*", "", source).replace("[[nodiscard]]", "")
            return re.sub(r"\s+", "", source)

        headers = [compact(path.read_text(encoding="utf-8"))
                   for path in (sitegen.ROOT / "sdk/include/bedrock/sdk").glob("*.hpp")]
        for slug, (_, _, signatures, _, _) in sitegen.CPP.items():
            text = compact(signatures)
            start = braces = parens = 0
            for i, char in enumerate(text):
                braces += (char == "{") - (char == "}")
                parens += (char == "(") - (char == ")")
                if char == ";" and not braces and not parens:
                    declaration = text[start:i]
                    with self.subTest(page=slug, declaration=declaration):
                        self.assertTrue(any(declaration + end in header
                                            for header in headers for end in (";", "{", ":")))
                    start = i + 1
            self.assertEqual(start, len(text), slug)
