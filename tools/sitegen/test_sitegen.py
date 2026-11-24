"""Regression tests for the generated reference and guides."""

import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

import sitegen


class LoggingReferenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.modules = {mod["name"]: mod for mod in sitegen.collect_modules()}

    def test_compact_logger_declarations_are_separate_and_public(self):
        logging = self.modules["logging"]
        types = {t["name"]: t["members"] for t in logging["types"]}
        logger = {m["signature"] for m in types["Logger"]}
        for level in ("debug", "info", "warn", "error"):
            self.assertIn(f"void {level}(std::string_view message);", logger)
            self.assertIn(f"template<class... Parts> void {level}(Parts&&... parts);", logger)
        self.assertIn("~Logger();", logger)
        self.assertIn("LogLevel level() const noexcept;", logger)
        sink = {m["signature"]: m for m in types["FileLogSink"]}
        self.assertIn("void close() noexcept;", sink)
        self.assertIn("does not detach", sink["void attach(Logger& logger) noexcept;"]["doc"])
        self.assertFalse(any("Impl" in signature for signature in logger | sink.keys()))

    def test_only_namespace_level_functions_are_listed(self):
        text = """
namespace bedrock::sdk {
namespace detail { void hidden(); }
template<class T>
struct Box { void member(); };
// Public description.
void run(void (*cb)(int), const char* text = "};") noexcept;
}
"""
        self.assertEqual(sitegen.parse_free_functions(text), [{
            "kind": "function",
            "signature": 'void run(void (*cb)(int), const char* text = "};") noexcept;',
            "doc": "Public description.",
        }])

    def test_html_and_xml_contain_content_log_and_legacy_api(self):
        modules = list(self.modules.values())
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            sitegen.render_api_index(out, modules)
            sitegen.render_module_page(out, modules, self.modules["content_log"])
            sitegen.render_module_page(out, modules, self.modules["logging"])
            sitegen.write_xml(out, modules, [])
            index = (out / "api/index.html").read_text(encoding="utf-8")
            self.assertIn('id="logging"', index)
            self.assertIn("Logging &amp; Content Log", index)
            self.assertIn("ContentLog::write", index)
            self.assertIn('href="content_log.html"', index)
            content = (out / "api/content_log.html").read_text(encoding="utf-8")
            self.assertIn('href="logging.html"', content)
            self.assertIn("[[nodiscard]] static bool available() noexcept;", content)
            self.assertIn("static void write(LogLevel level, std::string_view tag, std::string_view message) noexcept;", content)
            logging = (out / "api/logging.html").read_text(encoding="utf-8")
            self.assertIn("void emitContentLog(LogLevel level, std::string_view tag, std::string_view message) noexcept;", logging)
            xml = ET.parse(out / "api.xml")
            self.assertIsNotNone(xml.find(".//module[@name='logging']/functions/function"))

    def test_nested_definition_does_not_print_private_members(self):
        members = sitegen.parse_members(
            "public: class Nested { private: int hidden; }; void tick();", True)
        self.assertEqual([m["signature"] for m in members], ["class Nested;", "void tick();"])


class ReadabilityTest(unittest.TestCase):
    def test_guide_renders_safe_markup_and_unique_heading_links(self):
        toc, body = sitegen.guide_blocks("""# Title

## API & usage
See [example](https://example.com/) and `<value>`.
- One
- Two

## API & usage
```ts
const tag = "<script>";
## Not a heading
```
""")
        self.assertIn('href="#api-usage"', toc)
        self.assertIn('href="#api-usage-2"', toc)
        self.assertIn('id="api-usage-2"', body)
        self.assertIn('<a href="https://example.com/">example</a>', body)
        self.assertIn("<code>&lt;value&gt;</code>", body)
        self.assertIn("<ul><li>One</li><li>Two</li></ul>", body)
        self.assertIn('aria-label="ts example"', body)
        self.assertIn("&lt;script&gt;", body)
        self.assertNotIn('id="not-a-heading"', body)

    def test_scriptclient_is_on_site_and_legacy_guide_link_is_unchanged(self):
        articles = sitegen.collect_docs()
        article = next(art for art in articles if art["id"] == "scriptclient")
        self.assertEqual(article["category"], "scriptclient")
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            sitegen.render_index(out, [], articles)
            sitegen.render_scriptclient(out, article)
            index = (out / "index.html").read_text(encoding="utf-8")
            self.assertIn('id="guides"', index)
            self.assertIn('id="scriptclient"', index)
            self.assertIn('href="scriptclient.html"', index)
            self.assertIn(f'href="{sitegen.GITHUB_BASE}/blob/main/docs/client-api-quickstart.md"', index)
            guide = (out / "scriptclient.html").read_text(encoding="utf-8")
            for text in ('aria-label="Breadcrumb"', 'class="page-toc"', "EffectFake", "BEDROCK_SDK_MODS_DIR"):
                self.assertIn(text, guide)

    def test_api_toc_and_signature_density(self):
        modules = sitegen.collect_modules()
        current = next(module for module in modules if module["name"] == "logging")
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            (out / "api").mkdir()
            sitegen.render_module_page(out, modules, current)
            page = (out / "api/logging.html").read_text(encoding="utf-8")
            self.assertIn('href="#logger"', page)
            self.assertIn('id="logger"', page)
            self.assertIn('href="../index.html#guides"', page)
            self.assertIn('aria-label="LogLevel signatures"', page)
            self.assertIn('<table class="signature-table">', page)
            self.assertIn('<table class="signature-table has-description">', page)


if __name__ == "__main__":
    unittest.main()
