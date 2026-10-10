import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("check_svg_icons", Path(__file__).parents[1] / "check-svg-icons.py")
icons = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = icons
spec.loader.exec_module(icons)


class SvgScanTests(unittest.TestCase):
    def fixture(self, source):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        (root / "assets/icons/lucide").mkdir(parents=True)
        (root / "src").mkdir()
        (root / "src/example.cpp").write_text(source, encoding="utf-8")
        (root / "assets/icons/lucide/present.svg").write_text('<svg xmlns="http://www.w3.org/2000/svg"/>', encoding="utf-8")
        return root

    def test_missing_indirect_reference_fails_and_retains_location(self):
        report = icons.scan(self.fixture('Icon("lucide/missing"); SvgIcon("lucide/present");'))
        self.assertEqual(report["icon_count"], 2)
        self.assertEqual(report["failures"][0]["name"], "lucide/missing")
        self.assertEqual(report["failures"][0]["locations"], ["src/example.cpp:1"])

    def test_comments_are_ignored_and_nested_arguments_are_scanned(self):
        report = icons.scan(self.fixture('// SvgIcon("lucide/missing")\n'
            '/* "lucide/old" */ SvgIcon(on ? QStringLiteral("lucide/present") : QLatin1String("lucide/present"));'))
        self.assertEqual(report["failures"], [])
        self.assertEqual(report["icon_count"], 1)

    def test_unknown_direct_name_and_broken_svg_are_rejected(self):
        root = self.fixture('SvgIcon("other/missing"); SvgIcon("lucide/present");')
        (root / "assets/icons/lucide/present.svg").write_text("<svg>", encoding="utf-8")
        self.assertEqual(len(icons.scan(root)["failures"]), 2)

    def test_partial_computed_literal_is_not_silently_accepted(self):
        self.assertTrue(icons.scan(self.fixture('SvgIcon("lucide/" + dynamic);'))["failures"])

    def test_dynamic_expression_coverage_is_disclosed(self):
        report = icons.scan(self.fixture('SvgIcon(name); SvgIcon("lucide/present");'))
        self.assertEqual(report["failures"], [])
        self.assertEqual(report["dynamic_calls"], ["src/example.cpp:1"])


if __name__ == "__main__":
    unittest.main()
