"""原生 loader 准备合同的原创临时目录回归，不使用游戏资源或注册表。"""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("p1_native_loader", Path(__file__).with_name("p1-native-loader.py"))
loader = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(loader)


class NativeLoaderTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="forge-p1-loader-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.game = self.root / "game"
        self.host = self.game / "YDWE-p1"
        self.original = self.root / "original"
        self.evidence = self.root / "evidence"
        for directory in (self.game, self.host, self.original, self.evidence):
            directory.mkdir(parents=True, exist_ok=True)
        self.reference = self.original / "worldedit.exe"
        self.reference.write_bytes(b"original loader identity\x00\xff\x10")
        self.loader_sha = loader.sha_bytes(self.reference.read_bytes())
        rows = []
        for name in loader.SENTINELS:
            file = self.game / name
            file.write_bytes(("original game sentinel " + name).encode("ascii"))
            rows.append({"path": name, **loader.file_identity(file)})
        self.game_marker = self.game / "p1-sandbox.json"
        self.write_json(self.game_marker, {"schema": "p1-game-sandbox-v1", "status": "prepared",
                                         "source_unchanged": True, "source": str(self.original),
                                         "sandbox": str(self.game), "files": rows})
        for name in loader.HOST_REQUIRED:
            file = self.host / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(("fixture host dependency " + name).encode("ascii"))
        (self.host / "bin/worldedit.exe").write_bytes(self.reference.read_bytes())
        self.host_marker = self.host / "p1-host-assembly.local.json"
        self.write_json(self.host_marker, {"host": str(self.host), "overlay_files": 1,
                                         "source_unchanged": True, "base_ui_preserved": True,
                                         "base_include_preserved": True,
                                         "registry_modified": False, "ydwe_started": False})
        self.args = SimpleNamespace(game_root=str(self.game), host_root=str(self.host),
                                    loader_reference=str(self.reference), expected_loader_sha256=self.loader_sha,
                                    receipt=str(self.evidence / "input.local.json"))

    @staticmethod
    def write_json(path, value):
        path.write_text(json.dumps(value), encoding="utf-8")

    def inspected(self):
        result = loader.inspect(self.args)
        self.args.input_inventory = self.args.receipt
        self.args.expected_input_sha256 = result["sha256"]
        self.args.receipt = str(self.evidence / "prepared.local.json")
        return result

    def receipt(self):
        return json.loads(Path(self.args.receipt).read_text(encoding="utf-8"))

    def assert_no_outputs(self):
        self.assertFalse((self.game / loader.LOADER_NAME).exists())
        self.assertFalse((self.game / loader.CONFIG_NAME).exists())

    def test_success_copies_exact_loader_config_and_binds_every_file(self):
        before_environment = dict(os.environ)
        inspection = self.inspected()
        self.assert_no_outputs()
        self.assertFalse((self.host / "logs").exists())
        self.assertFalse(json.loads(Path(self.args.input_inventory).read_text())['launched'])
        result = loader.prepare(self.args)
        receipt = self.receipt()
        self.assertEqual(result["status"], "prepared")
        self.assertEqual((self.game / loader.LOADER_NAME).read_bytes(), self.reference.read_bytes())
        self.assertEqual((self.game / loader.CONFIG_NAME).read_bytes(), loader.EDITOR_LOADER_CONFIG)
        # 此值独立读取已审查原入口的固定模板所得，而不是从本次输出生成。
        self.assertEqual(receipt["config"]["sha256"], "8a66aa443cb364fb9e2c76548e75efe537c0f50190081a9f66a6c8ae95e5e10e")
        self.assertTrue(receipt["sourceUnchanged"])
        self.assertEqual(len(receipt["dependencies"]), inspection["files"] + 2)
        self.assertTrue(receipt["hostLogs"]["created"])
        self.assertIn("logs", receipt["directories"]["host"])
        self.assertFalse(receipt["launched"])
        self.assertFalse(receipt["registryWritten"])
        self.assertFalse(receipt["nativeSaveValidated"])
        self.assertEqual(dict(os.environ), before_environment)
        self.assertNotIn("YDWE-p1", receipt["directories"]["game"])
        self.assertEqual({row["root"] for row in receipt["dependencies"]}, {"game", "host"})

    def test_existing_plain_logs_are_not_recreated_or_overwritten(self):
        (self.host / "logs").mkdir()
        (self.host / "logs" / "preserved.txt").write_bytes(b"existing evidence")
        self.inspected()
        loader.prepare(self.args)
        self.assertFalse(self.receipt()["hostLogs"]["created"])
        self.assertEqual((self.host / "logs" / "preserved.txt").read_bytes(), b"existing evidence")

    def test_wrong_expected_loader_sha_refuses_before_output(self):
        self.args.expected_loader_sha256 = "0" * 64
        with self.assertRaisesRegex(ValueError, "SHA"):
            loader.inspect(self.args)
        self.assert_no_outputs()
        self.assertFalse(Path(self.args.receipt).exists())

    def test_reference_must_match_original_owned_loader_bytes(self):
        self.reference.write_bytes(b"different original")
        with self.assertRaisesRegex(ValueError, "逐字节"):
            loader.inspect(self.args)
        self.assert_no_outputs()

    def test_game_member_corruption_refuses_even_when_five_names_exist(self):
        (self.game / "game.dll").write_bytes(b"corrupt game")
        with self.assertRaisesRegex(ValueError, "身份变化"):
            loader.inspect(self.args)
        self.assert_no_outputs()

    def test_manifest_requires_all_five_sentinels_not_untrusted_partial_manifest(self):
        value = json.loads(self.game_marker.read_text())
        value["files"] = value["files"][:-1]
        self.write_json(self.game_marker, value)
        with self.assertRaisesRegex(ValueError, "五个"):
            loader.inspect(self.args)

    def test_manifest_game_and_original_source_must_be_independent(self):
        value = json.loads(self.game_marker.read_text())
        value["source"] = str(self.game / "source")
        self.write_json(self.game_marker, value)
        with self.assertRaisesRegex(ValueError, "互为父子"):
            loader.inspect(self.args)

    def test_assembly_requires_owned_unstarted_full_copy(self):
        for key, value in (("host", str(self.original)), ("overlay_files", True),
                           ("base_ui_preserved", False), ("ydwe_started", True), ("registry_modified", True)):
            with self.subTest(key=key):
                original = self.host_marker.read_bytes()
                marker = json.loads(original)
                marker[key] = value
                self.write_json(self.host_marker, marker)
                with self.assertRaises(ValueError):
                    loader.inspect(self.args)
                self.host_marker.write_bytes(original)

    def test_missing_native_host_dependency_refuses(self):
        (self.host / "bin/StormLib.dll").unlink()
        with self.assertRaisesRegex(ValueError, "成员缺失"):
            loader.inspect(self.args)

    def test_game_unrecorded_executable_and_host_root_shadow_refuse(self):
        for root, name in ((self.game, "surprise.asi"), (self.game, "ydbase.dll"), (self.host, "YDDllFixer.dll")):
            with self.subTest(name=name, root=root.name):
                file = root / name
                file.write_bytes(b"unowned shadow")
                with self.assertRaises(ValueError):
                    loader.inspect(self.args)
                file.unlink()

    def test_wrong_input_inventory_pin_refuses_without_output(self):
        self.inspected()
        self.args.expected_input_sha256 = "0" * 64
        with self.assertRaisesRegex(ValueError, "inventory 显式 SHA"):
            loader.prepare(self.args)
        self.assert_no_outputs()
        self.assertFalse(Path(self.args.receipt).exists())

    def test_added_host_file_after_inspect_refuses_before_writes(self):
        self.inspected()
        (self.host / "bin/new-plugin.dll").write_bytes(b"unexpected module")
        with self.assertRaisesRegex(ValueError, "完整文件/目录清单不同"):
            loader.prepare(self.args)
        self.assert_no_outputs()

    def test_changed_dependency_after_inspect_refuses_before_writes(self):
        self.inspected()
        (self.host / "script/common/config.lua").write_bytes(b"changed path policy")
        with self.assertRaisesRegex(ValueError, "完整文件/目录清单不同"):
            loader.prepare(self.args)
        self.assert_no_outputs()

    def test_added_empty_directory_after_inspect_refuses(self):
        self.inspected()
        (self.host / "injected-empty").mkdir()
        with self.assertRaisesRegex(ValueError, "完整文件/目录清单不同"):
            loader.prepare(self.args)
        self.assert_no_outputs()

    def test_existing_loader_or_config_case_alias_refuses_and_preserves_bytes(self):
        for name in (loader.LOADER_NAME.upper(), loader.CONFIG_NAME.upper()):
            with self.subTest(name=name):
                file = self.game / name
                file.write_bytes(b"existing external bytes")
                with self.assertRaises(ValueError):
                    loader.inspect(self.args)
                self.assertEqual(file.read_bytes(), b"existing external bytes")
                file.unlink()

    def test_receipt_must_be_new_and_outside_inputs_and_original_install(self):
        for output in (self.game / "receipt.json", self.host / "receipt.json", self.original / "receipt.json"):
            with self.subTest(output=str(output)):
                self.args.receipt = str(output)
                with self.assertRaises(ValueError):
                    loader.inspect(self.args)
                self.assertFalse(output.exists())
        self.args.receipt = str(self.evidence / "exists.json")
        Path(self.args.receipt).write_bytes(b"preserve evidence")
        with self.assertRaisesRegex(ValueError, "独占新路径"):
            loader.inspect(self.args)
        self.assertEqual(Path(self.args.receipt).read_bytes(), b"preserve evidence")

    def test_hardlinked_game_input_and_reference_refuse(self):
        for file in (self.game / "war3.mpq", self.reference):
            with self.subTest(file=str(file)):
                alias = self.root / "hardlink"
                os.link(file, alias)
                with self.assertRaisesRegex(ValueError, "hardlink"):
                    loader.inspect(self.args)
                alias.unlink()

    def test_symlink_in_host_tree_is_rejected(self):
        link = self.host / "script/shadow"
        try:
            link.symlink_to(self.original, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"本机无创建临时 symlink 权限: {error}")
        with self.assertRaisesRegex(ValueError, "链接|reparse"):
            loader.inspect(self.args)
        link.unlink()

    def test_source_reference_cannot_alias_owned_host(self):
        self.args.loader_reference = str(self.host / "bin/worldedit.exe")
        with self.assertRaisesRegex(ValueError, "原安装参考"):
            loader.inspect(self.args)

    def test_invalid_relative_or_absolute_paths_refuse(self):
        for name in ("../Storm.dll", "C:/Storm.dll", "bin/worldedit.exe:stream", "bin//x", "bin/trailing."):
            with self.subTest(name=name), self.assertRaises(ValueError):
                loader.relative(name)
        for name in ("relative/path", str(self.game / "a:b"), "//server/share", str(self.game / "dot.")):
            with self.subTest(name=name), self.assertRaises(ValueError):
                loader.plain(name)

    def test_failed_copy_retains_partial_loader_and_failure_receipt(self):
        self.inspected()
        def failed_copy(incoming, outgoing, **kwargs):
            outgoing.write(b"partial bytes")
            raise OSError("injected fixture copy failure")
        with patch.object(loader.shutil, "copyfileobj", failed_copy), self.assertRaisesRegex(OSError, "fixture"):
            loader.prepare(self.args)
        self.assertEqual((self.game / loader.LOADER_NAME).read_bytes(), b"partial bytes")
        self.assertFalse((self.game / loader.CONFIG_NAME).exists())
        self.assertEqual(self.receipt()["status"], "failed")
        self.assertFalse(self.receipt()["sourceUnchanged"])
        self.assertEqual(self.reference.read_bytes(), b"original loader identity\x00\xff\x10")

    def test_racing_config_creation_is_preserved_by_create_new(self):
        self.inspected()
        real_copy = loader.shutil.copyfileobj
        def racing_copy(incoming, outgoing, **kwargs):
            real_copy(incoming, outgoing, **kwargs)
            (self.game / loader.CONFIG_NAME).write_bytes(b"new external config")
        with patch.object(loader.shutil, "copyfileobj", racing_copy), self.assertRaises(FileExistsError):
            loader.prepare(self.args)
        self.assertEqual((self.game / loader.CONFIG_NAME).read_bytes(), b"new external config")
        self.assertEqual(self.receipt()["status"], "failed")
        self.assertEqual(self.receipt()["createdFiles"], [str(self.game / loader.LOADER_NAME)])

    def test_after_copy_source_changes_produce_failed_retained_evidence(self):
        self.inspected()
        real_copy = loader.shutil.copyfileobj
        def mutate_source(incoming, outgoing, **kwargs):
            real_copy(incoming, outgoing, **kwargs)
            self.reference.write_bytes(b"source changed outside operation")
        with patch.object(loader.shutil, "copyfileobj", mutate_source), self.assertRaisesRegex(ValueError, "原安装 loader"):
            loader.prepare(self.args)
        self.assertEqual(self.receipt()["status"], "failed")
        self.assertTrue((self.game / loader.LOADER_NAME).exists())
        self.assertTrue((self.game / loader.CONFIG_NAME).exists())

    def test_json_size_budget_and_malformed_expected_sha_refuse(self):
        with patch.object(loader, "MAX_JSON_BYTES", 8), self.assertRaisesRegex(ValueError, "JSON"):
            loader.inspect(self.args)
        self.args.expected_loader_sha256 = "bogus"
        with self.assertRaisesRegex(ValueError, "64 位"):
            loader.inspect(self.args)


if __name__ == "__main__":
    unittest.main(verbosity=2)
