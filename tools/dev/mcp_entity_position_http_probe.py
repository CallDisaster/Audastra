"""C4 普通 classic 单位 XYZ 原字段事务、保存与独立 HTTP 回读探针。

只证明实际执行的原 fieldEdits 保存和完整 native 字节回读；不是 GUI/native Save
或游戏验收。新证据目录必须不存在，源归档与源工作区保持，复用冻结 HTTP/CLI helper。
"""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct

from mcp_object_http_probe import Evidence, extract_members, inventory, json_sha, list_members, normalized_member, require, safe_member, sha
from mcp_variables_http_probe import context
from mcp_entities_http_probe import HEX_SHA, change, fields, prepare, query_entities, raw_f32, unsigned
from mcp_placements_http_probe import (
    PROTECTED_MEMBERS, UNIT_MEMBER, PlacementHttpServer, begin, check_unit_query, f32, read_units, read_units_bytes,
)


AXES = ("x", "y", "z")
ORIGINAL_WRITABLE = {"owner", "facing", "hit_points", "mana"}


class PositionHttpServer(PlacementHttpServer):
    def start(self):
        super().start()
        listed = json.loads((self.evidence.output / f"{self.label}.tools-list.json").read_text(encoding="utf-8"))
        require(len(listed["tools"]) == len({tool["name"] for tool in listed["tools"]}) == 42,
                "C4 must retain 42 unique tool names")
        self.evidence.check(f"{self.label}-actual-42-tools-no-new-position-tool", {"count": 42})
        return self


def check_position_query(rows, native):
    require(len(rows) == native["count"], "C4 query/native row count differs")
    normalized = copy.deepcopy(rows)
    for actual, canonical, raw in zip(rows, normalized, native["rows"]):
        table = fields(actual)
        writable = {name for name, field in table.items() if field["writable"]}
        expected = ORIGINAL_WRITABLE | (set(AXES) if raw["rawcode"] != b"sloc" else set())
        require(writable == expected and len(writable) == (4 if raw["rawcode"] == b"sloc" else 7),
                "C4 ordinary/sloc shared row guard changed another field or hid coordinate permission")
        require(actual.get("locator_scope") == "snapshot-revision" and actual.get("source_index") == str(raw["index"]),
                "C4 row metadata lost original native index/snapshot scope")
        # 先严格证明新权限；仅复制DTO的XYZ权限归原值，保留所有真实value/raw/identity。
        for field in canonical["fields"]:
            if field["id"] in AXES:
                field["writable"] = False
    check_unit_query(normalized, native)


def expected_position_bytes(source, selected, values):
    require(selected["rawcode"] != b"sloc" and len(values) == 3, "only an ordinary unit may have XYZ changed")
    values = [f32(value) for value in values]
    require(all(abs(value) <= 1000000 for value in values), "independent XYZ exceeds finite native bounds")
    expected = bytearray(source["data"])
    for axis, value in enumerate(values):
        struct.pack_into("<f", expected, selected["start"] + 8 + axis * 4, value)
    return bytes(expected)


def position_proof(source, candidate, selected, values):
    expected = expected_position_bytes(source, selected, values)
    require(candidate["data"] == expected and candidate["count"] == source["count"] and
            len(candidate["data"]) == len(source["data"]), "whole native differs from the independent exact three-float position plan")
    start = selected["start"] + 8
    changed = [index for index, pair in enumerate(zip(source["data"], candidate["data"])) if pair[0] != pair[1]]
    require(changed and all(start <= index < start + 12 for index in changed), "position write changed native outside the 12 authorized bytes")
    require(candidate["data"][:16] == source["data"][:16], "position changed native header/count")
    for before, after in zip(source["rows"], candidate["rows"]):
        require(before["row_key"] == after["row_key"] and before["creation_id"] == after["creation_id"] and
                before["rawcode"] == after["rawcode"], "position changed native identity/order")
        if before["index"] == selected["index"]:
            require(before["bytes"][:8] == after["bytes"][:8] and before["bytes"][20:] == after["bytes"][20:],
                    "position changed facing/scale/owner/nested fields or record tail")
        else:
            require(before["bytes"] == after["bytes"], "position changed another original record")
    return {"source_member_sha256": source["sha256"], "candidate_member_sha256": candidate["sha256"],
        "source_index": str(selected["index"]), "source_row_key": selected["row_key"], "native_bytes": len(source["data"]),
        "record_count": source["count"], "authorized_byte_window": [start, start + 11], "changed_byte_offsets": changed,
        "requested_xyz": [f32(value) for value in values], "requested_xyz_raw": [raw_f32(f32(value)) for value in values],
        "all_other_original_records_exact": True, "selected_record_except_xyz_exact": True,
        "complete_native_equals_independent_plan": True, "cpp_position_edit_logic_used_by_oracle": False}


def expected_rows(original, index, values):
    result = copy.deepcopy(original); values = [f32(value) for value in values]
    result[index]["position_world"] = values
    result[index]["position_world_raw_u32"] = [struct.unpack("<I", struct.pack("<f", value))[0] for value in values]
    for field in result[index]["fields"]:
        if field["id"] in AXES:
            value = values[AXES.index(field["id"])]; field.update({"raw": raw_f32(value), "value": value})
    return result


def request_xyz(reply, row, values):
    return {"kind": "units", **context(reply), "expected_member_sha256": reply["member"]["member_sha256"],
        "changes": [change(row, axis, format(f32(value), ".9g")) for axis, value in zip(AXES, values)]}


def apply_xyz(server, evidence, label, source, selected, values, source_rows):
    before_rows, before = query_entities(server, "units"); check_position_query(before_rows, source)
    require(before_rows == source_rows and before["member"]["member_sha256"] == source["sha256"], "XYZ batch did not start from exact original units")
    request = request_xyz(before, before_rows[selected["index"]], values)
    envelope = copy.deepcopy(server.envelope)
    edited = server.tool("entities_set_fields", request)
    rows, reply = query_entities(server, "units")
    native = read_units_bytes(expected_position_bytes(source, selected, values)); check_position_query(rows, native)
    require(rows == expected_rows(source_rows, selected["index"], values) and reply["member"]["member_sha256"] == native["sha256"] and
            context(reply) != context(before) and context(edited) == context(reply) and server.envelope == envelope,
            "XYZ batch changed another DTO/current native binding or lost original transaction")
    require(edited.get("changed") is True and edited.get("saved") is False and edited.get("runtime_verified") is False and
            edited.get("kind") == "units" and unsigned(edited.get("applied_fields"), "applied_fields") == 3 and
            unsigned(edited.get("changed_rows"), "changed_rows") == 1 and
            edited.get("before") == {"member": before["member"], "rows": [before_rows[selected["index"]]]} and
            edited.get("after") == {"member": reply["member"], "rows": [rows[selected["index"]]]} and
            edited.get("before_binding") == before["binding"] and isinstance(edited.get("changes"), dict), "XYZ complete field receipt differs")
    command = {key: str(unsigned(edited.get(key), key, key in ("session_identity", "command_identity"))) for key in
        ("session_identity", "command_identity", "session_revision_before", "session_revision_after")}
    require(int(command["session_revision_after"]) == int(command["session_revision_before"]) + 1, "XYZ batch advanced more than one actual Core revision")
    evidence.artifact(label + ".actual-http.json", {"request": request, "write": edited, "rows": rows, "reply": reply})
    evidence.check(label, {"command": command, "proof": position_proof(source, native, selected, values),
        "ordinary_writable_fields": 7, "sloc_writable_fields": 4, "full_dtos_exact": True})
    return rows, reply, native, request


def reject_unchanged(server, evidence, label, request, code, rows, reply):
    envelope = copy.deepcopy(server.envelope); revision = server.tool("get_revision", {})
    rejected = server.rejection("entities_set_fields", request, code)
    require(query_entities(server, "units") == (rows, reply) and server.tool("get_revision", {}) == revision and
            server.envelope == envelope, "XYZ refusal published prefix or changed unit/native/CAS/transaction")
    evidence.artifact(label + ".refusal.json", {"request": request, "actual_refusal": rejected})
    evidence.check(label, {"expected_code": code, "entire_unit_rows_member_sha_binding_revision_and_original_transaction_unchanged": True})


def exercise_refusals(server, evidence, rows, reply, selected, old_request):
    row = rows[selected["index"]]
    reject_unchanged(server, evidence, "old-xyz-cas-refused", old_request, "ENTITY_REVISION_CONFLICT", rows, reply)
    for label, value, code in [
        ("decimal-above-native-bound", "1000001", "MAP_FIELD_INPUT_INVALID"),
        ("decimal-before-float-rounding-bound", "1000000.001", "MAP_FIELD_INPUT_INVALID"),
        ("negative-decimal-before-float-rounding-bound", "-1000000.001", "MAP_FIELD_INPUT_INVALID"),
        ("raw-positive-over-bound", "f32:49742401", "MAP_FIELD_VALUE_INVALID"),
        ("raw-negative-over-bound", "f32:c9742401", "MAP_FIELD_VALUE_INVALID"),
        ("raw-positive-infinity", "f32:7f800000", "MAP_FIELD_VALUE_INVALID"),
        ("raw-nan", "f32:7fc00000", "MAP_FIELD_VALUE_INVALID")]:
        request = {"kind": "units", **context(reply), "expected_member_sha256": reply["member"]["member_sha256"],
                   "changes": [change(row, "z", value)]}
        reject_unchanged(server, evidence, label, request, code, rows, reply)
    request = {"kind": "units", **context(reply), "expected_member_sha256": reply["member"]["member_sha256"],
        "changes": [change(row, "x", format(f32(row["position_world"][0] + 1), ".9g")), change(row, "z", "f32:7f800000")]}
    reject_unchanged(server, evidence, "valid-prefix-invalid-raw-last-atomic-refusal", request, "MAP_FIELD_VALUE_INVALID", rows, reply)
    sloc = next((other for other in rows if other["rawcode_hex"] == b"sloc".hex()), None)
    require(sloc is not None, "source lacks a real sloc position refusal fixture")
    sloc_x = sloc["position_world"][0]
    legal_sloc_x = f32(sloc_x + 1 if sloc_x < 999999 else sloc_x - 1)
    require(abs(legal_sloc_x) <= 1000000 and legal_sloc_x != sloc_x, "sloc refusal must use a legal distinct coordinate")
    request["changes"] = [change(row, "x", format(f32(row["position_world"][0] + 1), ".9g")),
                          change(sloc, "x", format(legal_sloc_x, ".9g"))]
    reject_unchanged(server, evidence, "valid-prefix-sloc-xyz-last-atomic-refusal", request, "MAP_FIELD_POSITION_READ_ONLY", rows, reply)


def restore(server, evidence, source_rows, source_reply, native, label):
    server.rollback(); rows, reply = query_entities(server, "units"); check_position_query(rows, native)
    require(rows == source_rows and reply["member"] == source_reply["member"] and server.envelope is None,
            "original rollback did not restore every unit DTO/native member")
    evidence.check(label, {"entire_original_unit_table_and_member_sha_restored": True})


def save_and_reopen(args, env, evidence, servers, server, source_sha, source_members, audit,
                    original_rows, original_reply, native, expected, selected, values, after_rows):
    envelope = copy.deepcopy(server.envelope)
    require(envelope is not None, "XYZ save lacks the original explicit transaction")
    preview = server.tool("preview_edit", envelope); evidence.artifact("xyz.transaction.preview.json", preview)
    patches = preview.get("changes")
    require(isinstance(patches, list) and len(patches) == 1 and patches[0].get("kind") == "map-data" and
            normalized_member(patches[0].get("path", "")) == UNIT_MEMBER and
            patches[0].get("before_sha256") == native["sha256"] and patches[0].get("after_sha256") == expected["sha256"],
            "XYZ preview is not exactly the independent one-native patch")
    validation = server.tool("validate_edit", envelope)
    require(validation.get("satisfied") is True and validation.get("highest_proven_level", -1) >= 0 and server.envelope == envelope,
            "XYZ original transaction did not validate")
    evidence.check("original-xyz-preview-validation-exact-native", {"preview": preview, "validation": validation})
    candidate = args.output / ("mcp-entity-c4-position-candidate" + args.source.suffix.lower())
    require(not candidate.exists(), "XYZ candidate already exists")
    saved = server.tool("save_map_edit_candidate", {"source_map_path": str(args.source), "output_path": str(candidate),
        "expected_source_sha256": source_sha, **envelope, "script_target": "source"}, timeout=args.save_timeout)
    require(candidate.is_file() and saved.get("ok") is True and saved.get("candidate_kind") == "mpq-map-archive" and
            saved.get("source_sha256") == source_sha and saved.get("candidate_sha256") == sha(candidate) and
            saved.get("script_rebuilt") is True and saved.get("native_editor_verified") is False and saved.get("game_runtime_verified") is False and
            isinstance(saved.get("output_path"), str) and Path(saved["output_path"]).resolve(strict=True) == candidate and server.envelope == envelope,
            "XYZ save archive/source/transaction/evidence identity differs")
    for key, count in (("applied_field_edit_count", 3), ("applied_placement_count", 0), ("applied_terrain_stroke_count", 0)):
        require(type(saved.get(key)) is int and saved[key] == count, "XYZ save did not consume exactly three original field edits")
    edited = saved.get("edited_members")
    require(isinstance(edited, list) and len(edited) == 1 and normalized_member(edited[0].get("name", "")) == UNIT_MEMBER and
            edited[0].get("original_sha256") == native["sha256"] and edited[0].get("saved_sha256") == expected["sha256"],
            "XYZ save hid another native change")
    evidence.receipt.update({"saved": saved, "candidate": str(candidate)})
    evidence.check("original-save-real-three-field-position-candidate", saved)
    restore(server, evidence, original_rows, original_reply, native, "after-save-original-xyz-rollback-exact")
    server.close()
    members = list_members(args, env, evidence, candidate, "candidate")
    workspace = args.output / "candidate-workspace"
    manifest = extract_members(args, env, evidence, candidate, list(members.values()), workspace, "candidate")
    protected = {}
    for name in PROTECTED_MEMBERS:
        before, after = audit.get(name), manifest.get(name)
        require((name in source_members) == (before is not None) == (after is not None) and
                (before is None or before["sha256"] == after["sha256"]), "XYZ save changed protected member presence/SHA: " + name)
        protected[name] = {"present": before is not None, "source_sha256": before["sha256"] if before else None,
                           "candidate_sha256": after["sha256"] if after else None}
    evidence.check("wtg-wct-seven-object-natives-other-native-tiara-presence-and-sha-preserved", protected)
    actual = read_units(workspace / safe_member(members[UNIT_MEMBER])); proof = position_proof(native, actual, selected, values)
    require(actual["sha256"] == manifest[UNIT_MEMBER]["sha256"] == expected["sha256"], "CLI candidate native SHA differs")
    evidence.artifact("xyz.candidate.independent-whole-native-proof.json", proof)
    evidence.check("entire-cli-saved-native-only-original-row-xyz-twelve-byte-window", proof)
    reopened = PositionHttpServer(args, env, workspace, "candidate", evidence); servers.append(reopened); reopened.start()
    rows, reply = query_entities(reopened, "units"); check_position_query(rows, actual)
    require(rows == after_rows and reply["member"]["member_sha256"] == actual["sha256"] and reopened.envelope is None and
            context(reply)["project_identity"] != context(original_reply)["project_identity"] and
            reply["binding"]["source_sha256"] == reopened.record["initial_revision"]["revision"]["source_sha256"] and
            reopened.record["initial_revision"]["revision"]["ui_catalog_sha256"] == server.record["initial_revision"]["revision"]["ui_catalog_sha256"],
            "second owned service did not read the complete exact XYZ candidate with fresh identity/current row guard")
    source_tools = json.loads((args.output / "source.tools-list.json").read_text(encoding="utf-8"))
    require(source_tools == json.loads((args.output / "candidate.tools-list.json").read_text(encoding="utf-8")), "candidate tool schemas changed")
    evidence.artifact("xyz.candidate.actual-http-readback.json", {"rows": rows, "reply": reply})
    evidence.check("new-owned-http-complete-xyz-and-row-guard-saved-candidate-readback", {
        "candidate_archive_sha256": sha(candidate), "native_sha256": actual["sha256"], "rows": len(rows),
        "ordinary_writable_fields": 7, "sloc_writable_fields": 4, "original_row_locator_requeried": True})
    reopened.close()


def inputs():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("server", "cli", "source", "workspace", "ydwe-root", "war3-root", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--expected-source-sha256", required=True); parser.add_argument("--qt-bin", type=Path)
    for name, default in (("start", 60), ("request", 60), ("cli", 60), ("save", 900)):
        parser.add_argument("--" + name + "-timeout", type=float, default=default)
    args = parser.parse_args()
    require(HEX_SHA.fullmatch(args.expected_source_sha256), "source archive SHA must be lowercase hex")
    for key in ("start_timeout", "request_timeout", "cli_timeout", "save_timeout"):
        require(0 < getattr(args, key) <= 3600, "invalid timeout")
    args.output = args.output.resolve()
    require(not args.output.is_relative_to(args.workspace.resolve()), "evidence must be outside input workspace")
    args.output.mkdir(parents=True, exist_ok=False)
    return args


def main():
    args = inputs(); evidence = Evidence(args.output)
    evidence.receipt.update({"schema": "audastra-mcp-unit-position-c4-http-probe-v1",
        "scope": "classic-ordinary-unit-single-original-xyz-batch-row-guard-original-save-cli-whole-native-new-mcp",
        "whole_C_group_verified": False, "saved_position_verified": False, "row_locators_across_revisions_claimed_permanent": False})
    servers, source_before, workspace_before = [], None, None
    try:
        env, source_before, workspace_before = prepare(args, evidence)
        members = list_members(args, env, evidence, args.source, "source")
        require({UNIT_MEMBER, "war3map.wtg", "war3map.wct"} <= members.keys(), "source lacks actual unit/trigger natives")
        audit_dir = args.output / "source-member-hashes"
        audit = extract_members(args, env, evidence, args.source,
            [members[name] for name in (UNIT_MEMBER,) + PROTECTED_MEMBERS if name in members], audit_dir, "source-audit")
        for name, row in audit.items():
            require(sha(args.workspace / safe_member(members[name])) == row["sha256"], "source archive/workspace native mismatch")
        native = read_units(audit_dir / safe_member(members[UNIT_MEMBER]))
        selected = next((row for row in native["rows"] if row["rawcode"] != b"sloc" and
                         all(abs(value) < 900000 for value in row["position"])), None)
        require(selected is not None, "source has no ordinary unit with bounded distinct XYZ targets")
        values = [f32(value + delta) for value, delta in zip(selected["position"], (32, -16, 8))]
        require(all(before != after for before, after in zip(selected["position"], values)), "three requested axes must each have a net change")
        server = PositionHttpServer(args, env, args.workspace, "source", evidence); servers.append(server); server.start()
        original_rows, original_reply = query_entities(server, "units"); check_position_query(original_rows, native)
        require(original_reply["member"]["member_sha256"] == native["sha256"] and server.envelope is None and
                original_reply["binding"]["source_sha256"] == server.record["initial_revision"]["revision"]["source_sha256"], "source unit/member/aggregate identity differs")
        evidence.artifact("source-units.before.json", {"rows": original_rows, "reply": original_reply})
        begin(server)
        rows, reply, expected, old_request = apply_xyz(server, evidence, "first-real-three-xyz-fields-one-command", native, selected, values, original_rows)
        exercise_refusals(server, evidence, rows, reply, selected, old_request)
        restore(server, evidence, original_rows, original_reply, native, "pre-save-xyz-transaction-rollback-exact")
        begin(server)
        rows, reply, expected, _ = apply_xyz(server, evidence, "fresh-original-save-transaction-three-xyz-fields", native, selected, values, original_rows)
        save_and_reopen(args, env, evidence, servers, server, source_before, members, audit,
                        original_rows, original_reply, native, expected, selected, values, rows)
        require(len(servers) == 2 and all(item.process.poll() is not None and item.record.get("owned_process_terminated") is True for item in servers),
                "owned source/candidate processes were not actually cleaned")
        evidence.receipt["saved_position_verified"] = True; evidence.receipt["passed"] = True
        evidence.check("two-owned-http-services-actually-cleaned", {"count": 2, "exit_codes": [item.process.returncode for item in servers]})
        evidence.checkpoint("passed-independent-c4-xyz-original-save-and-exact-candidate-readback")
    except Exception as error:
        evidence.receipt["failed_stage"] = evidence.receipt["stage"]; evidence.receipt["error"] = evidence.scrub(str(error)); evidence.checkpoint("failed")
    finally:
        for server in reversed(servers):
            try: server.close()
            except Exception as error: evidence.receipt["cleanup_errors"].append({"server": server.label, "error": evidence.scrub(str(error))})
        try:
            if source_before is not None:
                after = sha(args.source); evidence.receipt.update({"source_sha256_after": after, "source_unchanged": after == source_before})
            if workspace_before is not None:
                after = inventory(args.workspace); evidence.artifact("source-workspace.after.json", after)
                evidence.receipt["source_workspace_unchanged"] = after == workspace_before
            require(all(item.process is None or item.process.poll() is not None for item in servers), "owned HTTP process remains running")
        except Exception as error: evidence.receipt["cleanup_errors"].append({"identity_recheck_error": evidence.scrub(str(error))})
        if evidence.receipt["cleanup_errors"] or not evidence.receipt.get("source_unchanged") or not evidence.receipt.get("source_workspace_unchanged"):
            evidence.receipt["passed"] = False; evidence.receipt["saved_position_verified"] = False
        evidence.checkpoint(); evidence.log.close()
    print(json.dumps({"passed": evidence.receipt["passed"], "receipt": str(args.output / "receipt.json"),
                      "candidate": evidence.receipt.get("candidate"), "error": evidence.receipt.get("error")}, ensure_ascii=False))
    return 0 if evidence.receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
