"""Regression tests for the generated C++ logging reference."""

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


if __name__ == "__main__":
    unittest.main()
