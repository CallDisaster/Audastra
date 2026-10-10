"""仅测公共候选合同/事务调用，不冒称 native codec、GUI 或真实地图验收。"""
import copy
import importlib.util
import json
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("matrix", Path(__file__).parents[1] / "p1-object-matrix.py")
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)


class PublicServer:
    def __init__(self):
        self.envelope = None
        self.revision = 1
        self.write_calls = []
        self.read_calls = []
        self.rollback_calls = 0
        self.fail_write = None
        self.fail_rollback = False
        self.objects, self.rows, self.choice_rows = {}, {}, {}
        self.before = None
        for index, kind in enumerate(matrix.KINDS):
            # 与旧 WP2 固定 ID 无关；upgrade 刻意为 map original override。
            object_id = f"Q{index:03d}"
            original = kind == "upgrade"
            self.objects[kind] = [{"id": object_id, "stable_id": str(100 + index), "name": "TRIGSTR_1",
                "source": "map_override" if original else "custom", "parent_id": None if original else f"P{index:03d}"}]
            key = {"field": f"F{index:03d}", "raw_key": f"source{index}", "display_name": "fixture",
                "type": "targetList", "minimum": "", "maximum": "", "ordered_list": True,
                "value": ["alpha", "beta"], "default_value": [], "overridden": True,
                "value_source": "map:" + object_id, "editable": True, "reason": ""}
            if kind == "ability":
                key.update(type="int", value=7, ordered_list=False, level=2, data_index=1, minimum="1", maximum="100")
            elif kind == "upgrade":
                key.update(type="intList", value=["4"], level=3, data_index=0)
            elif kind == "doodad":
                key.update(data_index=0)
            self.rows[kind] = [key]
            self.choice_rows[kind] = []

    def identity(self):
        return {"project_identity": "owned-workspace", "revision": str(self.revision)}

    def tool(self, name, arguments):
        if name == "get_revision":
            self.read_calls.append(name)
            return {"ok": True, "transaction_active": self.envelope is not None}
        kind = arguments["kind"]
        if name == "objects_set_fields":
            self.write_calls.append(copy.deepcopy(arguments))
            if self.fail_write == len(self.write_calls):
                raise matrix.MatrixError("real facade business rejection fixture")
            matrix.require(arguments["project_identity"] == "owned-workspace" and arguments["expected_revision"] == str(self.revision), "stale request")
            if self.before is None:
                self.before = copy.deepcopy(self.rows)
            for change in arguments["changes"]:
                matches = [r for r in self.rows[kind] if r["field"] == change["field"]
                    and r.get("level") == change.get("level") and r.get("variation") == change.get("variation")]
                matrix.require(len(matches) == 1, "fake rejected ambiguous setter")
                matches[0]["value"] = copy.deepcopy(change["value"])
            self.revision += 1
            self.envelope = {"transaction_id": "900", "expected_revision": {"generation": str(self.revision)}}
            return {"ok": True, **self.identity(), "transaction_active": True,
                "transaction_id": "900", "transaction_revision": self.envelope["expected_revision"]}
        self.read_calls.append(name)
        if name == "objects_list":
            rows, key = self.objects[kind], "items"
        elif name == "objects_get":
            rows, key = self.rows[kind], "fields"
            if arguments.get("fields"):
                rows = [r for r in rows if r["field"] in arguments["fields"]]
        elif name == "objects_field_options":
            rows, key = self.choice_rows[kind], "items"
        else:
            raise AssertionError("unsolicited tool " + name)
        page = arguments["page"]
        batch = rows[page["offset"]:page["offset"] + page["limit"]]
        result = {"ok": True, **self.identity(), key: copy.deepcopy(batch), "offset": page["offset"],
            "total": len(rows), "truncated": page["offset"] + len(batch) < len(rows)}
        if name == "objects_field_options":
            result["free_value"] = not rows
        return result

    def rollback(self):
        self.rollback_calls += 1
        if self.fail_rollback:
            raise matrix.MatrixError("rollback fixture refused")
        if self.before is not None:
            self.rows = self.before
            self.before = None
            self.envelope = None
            self.revision += 1


class ObjectMatrixTests(unittest.TestCase):
    def setUp(self):
        self.server = PublicServer()
        self.bindings = {"source_sha256": "a" * 64, "catalog": {"game_archives": [{"sha256": "b" * 64}]},
            "workspace_inventory_sha256": "c" * 64, "members": {}, "skin_members": {}}
        self.plan = matrix.build_plan(self.server, self.bindings)

    def test_seven_real_ids_sources_and_full_native_keys_are_kept(self):
        self.assertTrue(self.plan["ready"])
        self.assertEqual([c["kind"] for c in self.plan["cases"]], list(matrix.KINDS))
        ability = self.plan["cases"][2]
        self.assertEqual(ability["full_modification_key"], {"field": "F002", "level": 2, "variation": None, "data_index": 1})
        self.assertEqual(ability["command_address"], {"field": "F002", "level": 2})
        self.assertEqual(self.plan["cases"][-1]["object"]["source"], "map_override")
        self.assertIsNone(self.plan["cases"][-1]["object"]["parent_id"])
        self.assertIn("unsupported", self.plan["cases"][5]["variation_coverage"])
        self.assertEqual(self.plan["cases"][5]["full_modification_key"]["variation"], 0)
        self.assertEqual(self.plan["cases"][5]["command_address"], {"field": "F005"})
        self.assertFalse(any(name not in matrix.READ_TOOLS for name in self.server.read_calls))
        self.assertFalse(self.server.write_calls)

    def test_success_returns_original_transaction_without_save(self):
        receipt = matrix.apply_plan(self.server, self.plan, self.bindings)
        self.assertEqual(len(receipt["applied"]), 7)
        self.assertFalse(receipt["save_executed"])
        self.assertIsNotNone(self.server.envelope)
        self.assertEqual(len(self.server.write_calls), 7)
        self.assertEqual(self.server.rollback_calls, 0)
        self.server.rollback()
        self.assertTrue(matrix.verify_plan(self.server, self.plan)["passed"])

    def test_late_seventh_failure_restores_all_six_previous_changes(self):
        before = copy.deepcopy(self.server.rows)
        self.server.fail_write = 7
        with self.assertRaisesRegex(matrix.MatrixError, "EDIT_FAILED_ROLLED_BACK"):
            matrix.apply_plan(self.server, self.plan, self.bindings)
        self.assertEqual(self.server.rows, before)
        self.assertEqual(self.server.rollback_calls, 1)
        self.assertIsNone(self.server.envelope)
        self.assertEqual(len(self.server.write_calls), 7)

    def test_rollback_refusal_reports_actual_residual_changes(self):
        before = copy.deepcopy(self.server.rows)
        self.server.fail_write, self.server.fail_rollback = 7, True
        with self.assertRaisesRegex(matrix.MatrixError, "EDIT_FAILED_ROLLBACK_INCOMPLETE"):
            matrix.apply_plan(self.server, self.plan, self.bindings)
        self.assertNotEqual(self.server.rows, before)
        self.assertIsNotNone(self.server.envelope)

    def test_digest_tampering_fails_before_any_writes(self):
        self.plan["cases"][0]["new_typed_value"] = ["other"]
        with self.assertRaisesRegex(matrix.MatrixError, "PLAN_DIGEST_INVALID"):
            matrix.apply_plan(self.server, self.plan, self.bindings)
        self.assertFalse(self.server.write_calls)

    def test_source_catalog_change_fails_before_writes(self):
        bindings = copy.deepcopy(self.bindings)
        bindings["catalog"]["game_archives"][0]["sha256"] = "d" * 64
        with self.assertRaisesRegex(matrix.MatrixError, "SOURCE_OR_CATALOG_BINDING_CHANGED"):
            matrix.apply_plan(self.server, self.plan, bindings)
        self.assertFalse(self.server.write_calls)

    def test_parent_change_fails_all_preflight_before_writes(self):
        self.server.objects["destructable"][0]["parent_id"] = "pBAD"
        with self.assertRaisesRegex(matrix.MatrixError, "OBJECT_PARENT_OR_SOURCE_CHANGED"):
            matrix.apply_plan(self.server, self.plan, self.bindings)
        self.assertFalse(self.server.write_calls)

    def test_old_raw_list_order_change_fails_before_writes(self):
        self.server.rows["buff"][0]["value"].reverse()
        with self.assertRaisesRegex(matrix.MatrixError, "FIELD_VALUE_CHANGED"):
            matrix.apply_plan(self.server, self.plan, self.bindings)
        self.assertFalse(self.server.write_calls)

    def test_existing_transaction_is_never_rolled_back_by_runner(self):
        self.server.envelope = {"transaction_id": "foreign"}
        with self.assertRaisesRegex(matrix.MatrixError, "EXISTING_TRANSACTION_REFUSED"):
            matrix.apply_plan(self.server, self.plan, self.bindings)
        self.assertEqual(self.server.rollback_calls, 0)
        self.assertFalse(self.server.write_calls)

    def test_data_index_collision_is_blocked_without_inventing_data_argument(self):
        another = copy.deepcopy(self.server.rows["ability"][0])
        another["data_index"] = 2
        self.server.rows["ability"].append(another)
        with self.assertRaisesRegex(matrix.MatrixError, "FIELD_ADDRESS_AMBIGUOUS"):
            matrix.apply_plan(self.server, self.plan, self.bindings)
        self.assertFalse(self.server.write_calls)

    def test_missing_seventh_kind_remains_blocked(self):
        self.server.objects["doodad"] = []
        plan = matrix.build_plan(self.server, self.bindings)
        self.assertFalse(plan["ready"])
        with self.assertRaisesRegex(matrix.MatrixError, "PLAN_BLOCKED_NO_WRITES"):
            matrix.apply_plan(self.server, plan, self.bindings)
        self.assertFalse(self.server.write_calls)

    def test_skin_layer_cannot_be_silently_written(self):
        bindings = copy.deepcopy(self.bindings)
        bindings["skin_members"]["buff"] = ["war3mapskin.w3h"]
        plan = matrix.build_plan(self.server, bindings)
        self.assertFalse(plan["ready"])
        self.assertIn("Skin", plan["cases"][3]["reason"])
        self.assertFalse(self.server.write_calls)

    def test_raw_projection_ambiguous_values_are_refused(self):
        for value in ([], ["_"], ["bad\ufffdtext"]):
            row = {"value": value, "ordered_list": True}
            with self.assertRaisesRegex(matrix.MatrixError, "RAW_PROJECTION_AMBIGUOUS"):
                matrix.public_raw(row)
        with self.assertRaisesRegex(matrix.MatrixError, "RAW_PROJECTION_AMBIGUOUS"):
            matrix.public_raw({"value": True, "type": "bool"})

    def test_f32_raw_bits_keep_exact_original_float_value(self):
        raw = matrix.public_raw({"value": 0.30000001192092896, "type": "unreal"})
        self.assertEqual(raw, {"kind": "unreal", "f32_hex": "3e99999a"})

    def test_single_item_list_uses_actual_public_option(self):
        self.server.rows["item"][0]["value"] = ["AAAA"]
        self.server.choice_rows["item"] = [{"value": "BBBB", "id": "BBBB", "kind": "ability"}]
        plan = matrix.build_plan(self.server, self.bindings)
        self.assertEqual(plan["cases"][1]["new_typed_value"], ["BBBB"])
        self.assertEqual(plan["cases"][1]["expected_raw"]["text"], "AAAA")

    def test_no_progress_pagination_is_a_failure(self):
        class NoProgress:
            def tool(self, *_):
                return {"project_identity": "x", "revision": "1", "items": [], "offset": 0, "total": 1, "truncated": True}
        with self.assertRaisesRegex(matrix.MatrixError, "QUERY_NO_PROGRESS"):
            matrix.page_rows(NoProgress(), "objects_list", {}, "items", 4)

    def test_author_list_states_save_and_variation_limits(self):
        text = matrix.author_list(self.plan)
        self.assertIn("Q002", text)
        self.assertIn('"data_index":1', text)
        self.assertIn("variation", text)
        self.assertIn("save 未验", text)

    def test_planning_between_kinds_cannot_mix_revisions(self):
        class DriftingServer(PublicServer):
            def tool(self, name, arguments):
                if name == "objects_list" and arguments["kind"] == "item":
                    self.revision += 1
                return super().tool(name, arguments)
        server = DriftingServer()
        with self.assertRaisesRegex(matrix.MatrixError, "QUERY_REVISION_CHANGED"):
            matrix.build_plan(server, self.bindings)
        self.assertFalse(server.write_calls)

    def test_untouched_fields_do_not_allow_intervening_revision_before_apply(self):
        self.server.revision += 1
        with self.assertRaisesRegex(matrix.MatrixError, "PLANNING_REVISION_CHANGED"):
            matrix.apply_plan(self.server, self.plan, self.bindings)
        self.assertFalse(self.server.write_calls)
        self.assertEqual(self.server.rollback_calls, 0)

    def test_reopen_must_preserve_real_parent_and_stable_id(self):
        matrix.apply_plan(self.server, self.plan, self.bindings)
        self.server.objects["ability"][0]["parent_id"] = "PNEW"
        with self.assertRaisesRegex(matrix.MatrixError, "OBJECT_PARENT_OR_SOURCE_CHANGED"):
            matrix.verify_plan(self.server, self.plan, after=True)

    def test_real_mutation_uses_exact_f32_and_refuses_unrepresentable_step(self):
        row = {"type": "real", "value": 0.30000001192092896, "minimum": "0", "maximum": "1"}
        new = matrix.mutation(row, {"items": []})
        self.assertEqual(matrix.public_raw({**row, "value": new})["f32_hex"], "3ed9999a")
        with self.assertRaisesRegex(matrix.MatrixError, "NO_REVERSIBLE_TYPED_MUTATION"):
            matrix.mutation({**row, "value": 100000000.0, "maximum": "999999999"}, {"items": []})

    def test_after_readback_detects_signed_zero_wire_change(self):
        row = self.server.rows["ability"][0]
        row.update(type="unreal", value=-0.125, minimum="-1", maximum="1")
        plan = matrix.build_plan(self.server, self.bindings)
        self.assertEqual(plan["cases"][2]["new_raw"]["f32_hex"], "00000000")
        matrix.apply_plan(self.server, plan, self.bindings)
        self.server.rows["ability"][0]["value"] = -0.0
        with self.assertRaisesRegex(matrix.MatrixError, "FIELD_RAW_VALUE_CHANGED"):
            matrix.verify_plan(self.server, plan, after=True)

    def test_resealed_one_level_ability_contract_is_not_complex_coverage(self):
        self.plan["cases"][2]["full_modification_key"]["level"] = 1
        self.plan["contract_sha256"] = matrix.contract_digest(self.plan)
        with self.assertRaisesRegex(matrix.MatrixError, "PLAN_CASE_COVERAGE_INVALID"):
            matrix.validate_plan(self.plan)

    def test_changed_runner_requires_new_plan_even_with_resealed_digest(self):
        self.plan["implementation_sha256"] = "0" * 64
        self.plan["contract_sha256"] = matrix.contract_digest(self.plan)
        with self.assertRaisesRegex(matrix.MatrixError, "PLAN_IMPLEMENTATION_CHANGED"):
            matrix.validate_plan(self.plan)

    def test_candidate_reopen_can_change_workspace_but_not_executing_binary(self):
        candidate = copy.deepcopy(self.bindings)
        candidate["workspace_inventory_sha256"] = "f" * 64
        matrix.verify_bindings(self.plan, candidate)
        candidate["server_sha256"] = "e" * 64
        with self.assertRaisesRegex(matrix.MatrixError, "VERIFY_SOURCE_OR_CATALOG_CHANGED"):
            matrix.verify_bindings(self.plan, candidate)


if __name__ == "__main__":
    unittest.main()
