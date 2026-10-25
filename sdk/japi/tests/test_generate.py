import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("japi_codegen", ROOT / "tools/generate.py")
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)


class CodegenTests(unittest.TestCase):
    def setUp(self):
        self.manifest = gen.read_manifest(ROOT / "sample/demo.manifest.json")

    def test_demo(self):
        jass, cpp = gen.generate(self.manifest)
        self.assertIn('AjCallInt("demo.add", AjArgInt(a0) + AjArgInt(a1))', jass)
        self.assertIn('{"demo.add", "ii", AJ_INT, AJ_SYNC_COMMAND}', cpp)
        self.assertNotIn("WarVKModule", jass)

    def test_every_type(self):
        for t in "irsbd":
            data = copy.deepcopy(self.manifest)
            data["commands"][0]["arguments"] = t
            data["commands"][0]["result"] = t
            jass, cpp = gen.generate(data)
            self.assertIn("AjArg" + gen.SUFFIX[t], jass)
            self.assertIn(gen.KINDS[t], cpp)

    def test_invalid_contracts(self):
        for key, value in [("schema", "warvk.module.v1"), ("abi", True), ("abi", 1), ("name", "module"),
                           ("name", "../demo"), ("version", "a|b"), ("commands", {})]:
            data = copy.deepcopy(self.manifest)
            data[key] = value
            with self.assertRaises(ValueError):
                gen.validate(data)
        for key, value in [("name", "other.fn"), ("name", "demo.Bad"), ("name", "demo..x"),
                           ("arguments", "v"), ("arguments", "i" * 17), ("result", "x"), ("domain", "unsafe")]:
            data = copy.deepcopy(self.manifest)
            data["commands"][0][key] = value
            with self.assertRaises(ValueError):
                gen.validate(data)

    def test_duplicates_and_unknown_fields(self):
        with self.assertRaises(ValueError):
            json.loads('{"abi":1,"abi":2}', object_pairs_hook=gen.unique_object)
        data = copy.deepcopy(self.manifest)
        data["commands"].append(data["commands"][0])
        with self.assertRaises(ValueError):
            gen.validate(data)
        data = copy.deepcopy(self.manifest)
        data["allowUntrusted"] = True
        with self.assertRaises(ValueError):
            gen.validate(data)

    def test_symbol_mapping_is_injective(self):
        names = ["demo.a_b", "demo.a.b", "demo.a-b", "demo.a_ub", "demo.a_db", "demo.a_hb"]
        self.assertEqual(len(set(map(gen.wrapper_name, names))), len(names))

    def test_cli_refuses_overwrite(self):
        with tempfile.TemporaryDirectory(prefix="audastra-japi-codegen-test-") as temp:
            command = [sys.executable, str(ROOT / "tools/generate.py"), str(ROOT / "sample/demo.manifest.json"), "--out", temp]
            first = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(first.returncode, 0, first.stderr)
            before = (Path(temp) / "demo.j").read_bytes()
            second = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(second.returncode, 2)
            self.assertEqual(before, (Path(temp) / "demo.j").read_bytes())


if __name__ == "__main__":
    unittest.main()
