"""原创临时夹具回归；不调用真实编辑器、YDWE、游戏或私有地图。"""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), Path(__file__).with_name(name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


sandbox = load("p1-game-sandbox")
compare = load("p1-archive-compare")
roundtrip = load("p1-roundtrip")
driver = load("p1-map-data-driver")
support = load("p1-support")


class TemporaryFiles(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="audastra-p1-test-")
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def file(self, name, content=b"original-fixture"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path


class SandboxTests(TemporaryFiles):
    def test_default_profile_preserves_optional_local_archive_without_requiring_it(self):
        source = self.root / "source"
        for name in sandbox.DEFAULT_PROFILE["required"]:
            self.file("source/" + name, b"synthetic non-executable profile placeholder")
        absent = sandbox.prepare(source, self.root / "without-local", sandbox.DEFAULT_PROFILE)
        self.assertIn("War3Local.mpq", absent["missing_optional"])
        self.assertFalse((self.root / "without-local" / "War3Local.mpq").exists())
        local = self.file("source/War3Local.mpq", b"owned optional archive identity placeholder")
        before = support.sha256(local)
        present = sandbox.prepare(source, self.root / "with-local", sandbox.DEFAULT_PROFILE)
        self.assertNotIn("War3Local.mpq", present["missing_optional"])
        self.assertEqual((self.root / "with-local" / "War3Local.mpq").read_bytes(), local.read_bytes())
        self.assertEqual(support.sha256(local), before)
        localRow = next(row for row in present["files"] if row["path"] == "War3Local.mpq")
        self.assertEqual(localRow["sha256"], before)
        self.assertFalse(present["runtime_validated"])

    def test_missing_required_is_reported_and_not_copied(self):
        source = self.root / "source"
        source.mkdir()
        profile = {"required": ["Game.dll"], "optional": ["Movies"]}
        self.assertEqual(sandbox.inventory(source, profile)["missing_required"], ["Game.dll"])
        with self.assertRaisesRegex(ValueError, "缺必需"):
            sandbox.prepare(source, self.root / "target", profile)
        self.assertFalse((self.root / "target").exists())

    def test_copy_whitelist_hashes_case_and_source_unchanged(self):
        source = self.root / "source"
        game = self.file("source/War3.exe", b"own-not-a-real-executable")
        movie = self.file("source/Movies/a.txt", b"movie-placeholder")
        self.file("source/private-history.txt", b"not-copied")
        before = support.sha256(game)
        target = self.root / "target"
        receipt = sandbox.prepare(source, target, {"required": ["war3.exe"], "optional": ["Movies", "absent"]})
        self.assertEqual(receipt["status"], "prepared")
        self.assertEqual(receipt["missing_optional"], ["absent"])
        self.assertEqual(support.sha256(game), before)
        self.assertEqual((target / "Movies/a.txt").read_bytes(), movie.read_bytes())
        self.assertFalse((target / "private-history.txt").exists())
        self.assertFalse(receipt["runtime_validated"])
        with self.assertRaisesRegex(ValueError, "不存在"):
            sandbox.prepare(source, target, {"required": ["War3.exe"]})

    def test_nested_and_ancestor_outputs_are_rejected(self):
        source = self.file("source/game.dat").parent
        for target in (source / "nested", source, self.root):
            with self.assertRaisesRegex(ValueError, "分离"):
                sandbox.prepare(source, target, {"required": ["game.dat"]})

    def test_invalid_profiles_reject_traversal_duplicate_and_parent_overlap(self):
        for names in (["../a"], ["C:/a"], ["/a"], ["a:b"], ["A", "a"], ["Movies", "Movies/a"]):
            with self.subTest(names=names), self.assertRaises(ValueError):
                sandbox.profile_paths({"required": names})

    def test_reparse_source_is_rejected_when_supported(self):
        source = self.file("source/game.dat").parent
        link = source / "linked.dat"
        try:
            link.symlink_to(source / "game.dat")
        except OSError:
            self.skipTest("当前身份没有创建符号链接权限")
        with self.assertRaisesRegex(ValueError, "链接"):
            sandbox.inventory(source, {"required": ["linked.dat"]})

    def test_launch_requires_copied_launcher_and_untampered_files(self):
        source = self.file("source/game.exe").parent
        target = self.root / "target"
        sandbox.prepare(source, target, {"required": ["game.exe"]})
        game_map = self.file("private/source.w3x", b"synthetic-map")
        with self.assertRaisesRegex(ValueError, "清单"), patch.object(sandbox.subprocess, "Popen") as start:
            sandbox.launch(target, game_map, "other.exe", ["{map}"])
            start.assert_not_called()
        (target / "game.exe").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "变化"), patch.object(sandbox.subprocess, "Popen") as start:
            sandbox.launch(target, game_map, "game.exe", ["{map}"])
            start.assert_not_called()

    def test_launch_rejects_unrecorded_executable_module(self):
        source = self.file("source/game.exe").parent
        target = self.root / "target"
        sandbox.prepare(source, target, {"required": ["game.exe"]})
        game_map = self.file("private/source.w3x", b"synthetic-map")
        (target / "unlisted.dll").write_bytes(b"not-a-real-dll")
        with self.assertRaisesRegex(ValueError, "未记录"), patch.object(sandbox.subprocess, "Popen") as start:
            sandbox.launch(target, game_map, "game.exe", ["{map}"])
            start.assert_not_called()


class ArchiveTests(TemporaryFiles):
    def archive_fixture(self, opaque=False, omitted_special=False):
        left, right = self.file("a.w3x", b"left"), self.file("b.w3x", b"right")
        cli = self.file("fake-cli")
        cli, left, right = map(support.plain_path, (cli, left, right))
        hashes = {str(left): support.sha256(left), str(right): support.sha256(right)}
        payload = {str(left): b"same", str(right): b"changed"}

        def invoke(_cli, command, archive, *args, **kwargs):
            key = str(archive)
            if command == "archive-session-inspect":
                return f"sourceSha256={hashes[key]}\nnamedMemberCount=1\nopaqueMemberCount={int(opaque)}\n"
            if command == "mpq-hash-inspect":
                return (f"block entries=8 existing={2 + int(opaque)} unreferenced=0 aliased=0 anonymous={int(opaque)}\n"
                        + ("" if omitted_special else "  special: (listfile)\n"))
            if command == "mpq-list":
                return "war3map.wtg\ntotal=1 shown=1\n"
            if command == "mpq-extract":
                content = b"war3map.wtg\r\n"
                Path(args[1]).write_bytes(content)
                return f"extracted=(listfile) bytes={len(content)}\n"
            raise AssertionError(command)

        def identities(argv):
            key = argv[2]
            self.assertEqual(argv[4], hashes[key])
            return {"source_sha256": hashes[key], "members": [
                {"name": name, "state": "present", "bytes": str(len(payload[key])),
                 "sha256": __import__("hashlib").sha256(payload[key]).hexdigest()} for name in argv[6::2]]}

        return cli, left, right, invoke, identities

    def test_full_comparison_covers_special_and_flags_unexplained(self):
        cli, left, right, invoke, identities = self.archive_fixture()
        with patch.object(compare, "cli_run", invoke), patch.object(compare.support, "command_json", identities):
            report = compare.compare(cli, left, right)
        self.assertTrue(report["coverage_complete"])
        self.assertEqual(report["named_members_compared"], 2)
        self.assertEqual(report["unchanged_named_members"], 1)
        self.assertEqual(report["status"], "blocked")
        self.assertEqual(report["differences"][0]["member"], "war3map.wtg")
        self.assertFalse(report["original_editor_validated"])

    def test_opaque_blocks_cannot_be_marked_complete(self):
        cli, left, right, invoke, identities = self.archive_fixture(opaque=True)
        with patch.object(compare, "cli_run", invoke), patch.object(compare.support, "command_json", identities):
            report = compare.compare(cli, left, right)
        self.assertFalse(report["coverage_complete"])
        self.assertEqual(report["anonymous_blocks"], [1, 1])
        self.assertEqual(report["status"], "blocked")

    def test_missing_metadata_listing_is_rejected(self):
        cli, left, right, invoke, identities = self.archive_fixture(omitted_special=True)
        with patch.object(compare, "cli_run", invoke), patch.object(compare.support, "command_json", identities):
            with self.assertRaisesRegex(ValueError, "数量"):
                compare.compare(cli, left, right)

    def test_explanation_bound_to_exact_hashes_and_evidence(self):
        row = {"member": "x", "before": {"sha256": "old"}, "after": {"sha256": "new"}}
        for note in ({"member": "x", "before_sha256": "stale", "after_sha256": "new", "reason": "reason", "evidence": "test"},
                     {"member": "x", "before_sha256": "old", "after_sha256": "new", "reason": "reason", "evidence": ""}):
            with self.assertRaises(ValueError):
                compare.explain_differences([dict(row)], [note])
        note = {"member": "x", "before_sha256": "old", "after_sha256": "new", "reason": "fixture rewrite", "evidence": "original-fixture-1"}
        self.assertEqual(compare.explain_differences([dict(row)], [note])[0]["explanation_status"], "explained_pending_review")


class RoundtripTests(TemporaryFiles):
    def plan(self):
        return {"schema": "p1-roundtrip-plan-v1", "evidence_kind": "synthetic-fixture", "coverage": ["original field + accumulated round fields"],
                "open": ["fixture", "open", "{input}", "{round}"],
                "edit_save": ["fixture", "save", "{input}", "{output}", "{round}"],
                "readback": ["fixture", "readback", "{output}", "{round}"]}

    def runner(self, bad=None):
        def run(argv):
            action, source = argv[1], Path(argv[2])
            if action == "save":
                output, index = Path(argv[3]), int(argv[4])
                state = support.read_json(source)
                changes = {f"round-{index}": index}
                state.update(changes)
                if bad == "drop" and index == 2:
                    state.pop("round-1")
                support.write_json(output, state)
                return {"input_sha256": support.sha256(source), "output_sha256": support.sha256(output), "changes": changes}
            state = support.read_json(source)
            return {"source_sha256": "stale" if bad == "stale" else support.sha256(source), "state": state}
        return run

    def test_twenty_rounds_accumulate_and_never_claim_real_stage_gate(self):
        source = self.file("original.json", b'{"original":"preserved"}')
        before = source.read_bytes()
        report = roundtrip.run_roundtrip(source, self.root / "output", self.plan(), runner=self.runner())
        self.assertEqual(report["completed_rounds"], 20)
        self.assertEqual(report["status"], "synthetic-framework-passed")
        self.assertFalse(report["stage_d_twenty_round_gate_passed"])
        self.assertFalse(report["runtime_validated"])
        self.assertEqual(source.read_bytes(), before)
        final = support.read_json(self.root / "output/round-20.json")
        self.assertEqual(len(final), 21)
        self.assertEqual(final["round-1"], 1)

    def test_second_round_lost_data_fails_and_preserves_receipt(self):
        source = self.file("original.json", b'{"original":"preserved"}')
        with self.assertRaisesRegex(ValueError, "累计预期"):
            roundtrip.run_roundtrip(source, self.root / "output", self.plan(), runner=self.runner("drop"))
        report = support.read_json(self.root / "output/roundtrip-report.json")
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["completed_rounds"], 1)
        self.assertTrue(report["source_unchanged"])

    def test_stale_source_receipt_fails_before_edit(self):
        source = self.file("original.json", b'{"original":"preserved"}')
        with self.assertRaisesRegex(ValueError, "源SHA"):
            roundtrip.run_roundtrip(source, self.root / "output", self.plan(), runner=self.runner("stale"))
        self.assertFalse((self.root / "output/round-01.json").exists())

    def test_empty_semantics_and_unknown_placeholders_fail(self):
        with self.assertRaises(ValueError):
            roundtrip.validate_state({})
        with self.assertRaises(ValueError):
            roundtrip.render_argv(["fixture", "{unknown}"], {})
        self.assertFalse(roundtrip.same_state({"value": True}, {"value": 1}))

    def test_real_subprocess_driver_with_original_fixture_only(self):
        driver = self.file("driver.py", b'''import hashlib,json,pathlib,sys
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
action,p=sys.argv[1],pathlib.Path(sys.argv[2])
state=json.loads(p.read_text())
if action=="save":
 out=pathlib.Path(sys.argv[3]); n=int(sys.argv[4]); delta={"round-"+str(n):n}; state.update(delta)
 out.write_text(json.dumps(state)); result={"input_sha256":sha(p),"output_sha256":sha(out),"changes":delta}
else: result={"source_sha256":sha(p),"state":state}
print(json.dumps(result))
''')
        source = self.file("original.json", b'{"original":"preserved"}')
        plan = self.plan()
        for stage in ("open", "edit_save", "readback"):
            plan[stage] = [sys.executable, str(driver), *plan[stage][1:]]
        report = roundtrip.run_roundtrip(source, self.root / "subprocess-output", plan, rounds=20)
        self.assertEqual(report["completed_rounds"], 20)
        self.assertEqual(report["status"], "synthetic-framework-passed")


class MapDataDriverTests(TemporaryFiles):
    def test_generated_actual_plan_binds_map_paths_and_no_expected_state(self):
        plan = driver.make_plan("editor-fixture", "cli-fixture", "game-fixture", "ui-fixture")
        self.assertEqual(plan["evidence_kind"], "editor-cli")
        self.assertIn("{input}", plan["open"])
        self.assertIn("{output}", plan["readback"])
        self.assertNotIn("expected_state", json.dumps(plan))

    def replies(self, source, *, partial=False, bad_cursor=False):
        source_sha = support.sha256(source)
        members = [{"kind": kind, "state": "missing", "record_count": None} for kind in driver.KINDS]
        members[0].update(state="ready", record_count="1")
        members[1].update(state="partial" if partial else "ready", record_count="129")

        def query(kind, offset):
            rows = [{"source_index": str(i), "fields": [{"id": "x", "raw": "f32:00000000"}]} for i in range(offset, min(offset + 128, 129))]
            if kind == "settings":
                rows = [{"source_index": "0", "fields": [{"id": "map_name", "raw": "hex:6f726967696e616c"}]}]
            member = next(row for row in members if row["kind"] == kind)
            return {"ok": True, "binding": {"source_sha256": source_sha}, "members": members,
                    "member": member, "rows": rows,
                    "next_cursor": {"next_index": "0" if bad_cursor else "128"} if kind == "units" and offset == 0 else None}
        return query

    def test_all_pages_and_missing_members_are_explicit(self):
        source = self.file("original.fixture")
        source_sha, state = driver.semantic_state(None, source, self.replies(source))
        self.assertEqual(source_sha, support.sha256(source))
        self.assertEqual(state["units/record_count"], 129)
        self.assertEqual(state["units/128/x"], "f32:00000000")
        self.assertEqual(state["regions/member_state"], "missing")

    def test_partial_member_blocks_actual_driver(self):
        source = self.file("original.fixture")
        with self.assertRaisesRegex(ValueError, "未完整"):
            driver.semantic_state(None, source, self.replies(source, partial=True))

    def test_nonprogressing_page_is_rejected(self):
        source = self.file("original.fixture")
        with self.assertRaisesRegex(ValueError, "游标"):
            driver.semantic_state(None, source, self.replies(source, bad_cursor=True))

    def test_predicted_changes_are_exact_float32_and_default_sound(self):
        state = {"settings/record_count": 1, "units/record_count": 1, "units/0/x": "f32:3f800000",
                 "regions/record_count": 1, "regions/0/right": "f32:00000000", "cameras/record_count": 1,
                 "cameras/0/distance": "f32:00000000", "sounds/record_count": 1, "sounds/0/volume": "-1"}
        before = json.dumps(state, sort_keys=True)
        changes = driver.predicted_changes(state)
        self.assertEqual(changes["units/0/x"], "f32:42820000")
        self.assertEqual(changes["regions/0/right"], "f32:42000000")
        self.assertEqual(changes["cameras/0/distance"], "f32:42c80000")
        self.assertEqual(changes["sounds/0/volume"], "100")
        self.assertEqual(json.dumps(state, sort_keys=True), before)


class CommandCaptureTests(TemporaryFiles):
    def command(self, code):
        script = self.file("original-child.py", code.encode("utf-8"))
        return [sys.executable, "-B", str(script)]

    def test_python_json_contract_is_utf8_even_with_parent_gbk(self):
        argv = self.command('import json; print(json.dumps({"original":"\\u539f\\u59cb\\u5939\\u5177"},ensure_ascii=False))')
        with patch.dict(os.environ, {"PYTHONIOENCODING": "gbk:strict"}):
            reply = support.command_json(argv, cwd=self.root)
            self.assertEqual(reply, {"original": "原始夹具"})
            self.assertEqual(os.environ["PYTHONIOENCODING"], "gbk:strict")
        self.assertEqual(list(self.root.glob("forge-p1-command-failure-*")), [])

    def test_invalid_stdout_retains_actual_bytes_code_argv_and_offset(self):
        raw = b'{"original":true}\n\xbe\xff'
        argv = self.command('import os;os.write(1,' + repr(raw) + ');os.write(2,b"original stderr\\n")')
        with self.assertRaises(support.CommandCaptureError) as raised:
            support.command_json(argv, cwd=self.root, evidence_root=self.root)
        diagnostic = raised.exception.diagnostics
        self.assertEqual(diagnostic["exit_code"], 0)
        self.assertEqual(diagnostic["argv"], argv)
        self.assertEqual(diagnostic["decode_errors"][0]["stream"], "stdout")
        self.assertEqual(diagnostic["decode_errors"][0]["start"], 18)
        self.assertEqual(Path(diagnostic["streams"]["stdout"]["path"]).read_bytes(), raw)
        self.assertEqual(Path(diagnostic["streams"]["stderr"]["path"]).read_bytes(), b"original stderr\n")
        self.assertEqual(support.read_json(Path(diagnostic["directory"]) / "failure.local.json"), diagnostic)

    def test_invalid_stderr_blocks_valid_json_and_retains_both_streams(self):
        argv = self.command('import os;os.write(1,b"{\\"ok\\":true}");os.write(2,b"\\xbe")')
        with self.assertRaises(support.CommandCaptureError) as raised:
            support.command_json(argv, evidence_root=self.root)
        diagnostic = raised.exception.diagnostics
        self.assertEqual(diagnostic["decode_errors"][0]["stream"], "stderr")
        self.assertEqual(Path(diagnostic["streams"]["stdout"]["path"]).read_bytes(), b'{"ok":true}')
        self.assertEqual(Path(diagnostic["streams"]["stderr"]["path"]).read_bytes(), b"\xbe")

    def test_nonzero_exit_retains_valid_utf8_and_preserves_failure(self):
        argv = self.command('import os,sys;os.write(1,b"{\\"ok\\":true}");os.write(2,"原始错误".encode("utf-8"));sys.exit(7)')
        with self.assertRaises(support.CommandCaptureError) as raised:
            support.command_json(argv, evidence_root=self.root)
        diagnostic = raised.exception.diagnostics
        self.assertEqual(diagnostic["exit_code"], 7)
        self.assertEqual(diagnostic["decode_errors"], [])
        self.assertEqual(Path(diagnostic["streams"]["stderr"]["path"]).read_bytes().decode("utf-8"), "原始错误")

    def test_timeout_retains_partial_original_bytes_and_no_fake_exit(self):
        argv = self.command('import os,time;os.write(1,b"original partial stdout");os.write(2,b"original partial stderr");time.sleep(20)')
        with self.assertRaises(support.CommandCaptureError) as raised:
            support.run_command(argv, timeout=1, evidence_root=self.root)
        diagnostic = raised.exception.diagnostics
        self.assertEqual(diagnostic["reason"], "timeout")
        self.assertIsNone(diagnostic["exit_code"])
        self.assertEqual(Path(diagnostic["streams"]["stdout"]["path"]).read_bytes(), b"original partial stdout")
        self.assertEqual(Path(diagnostic["streams"]["stderr"]["path"]).read_bytes(), b"original partial stderr")


if __name__ == "__main__":
    unittest.main(verbosity=2)
