"""C2 独立 HTTP 连续绝对朝向保存与完整 classic W3do 字节回读。

复用已冻结探针的传输、原事务、CLI、脱敏和自有进程清理。
独立预期仅覆盖原行 facing 的四字节，不调用 C++ copy/rotate 算法。
只有实际运行全部保存与候选回读门禁后才产生通过证据，不宣称 GUI 或游戏验收。
"""

import copy
import hashlib
import json
from pathlib import Path
import struct

from mcp_placements_http_probe import (
    ENTITY_MEMBERS, Evidence, OBJECT_MEMBERS, PROTECTED_MEMBERS, PlacementHttpServer,
    UNIT_MEMBER, assert_receipt, begin, check_unit_query, context,
    expected_rotation_bytes, expected_rotation_rows, extract_members, f32, inputs,
    inventory, json_sha, list_members, normalized_member, placement_request,
    prepare, query_entities, raw_f32, read_units, read_units_bytes,
    reject_unchanged, require, restore_check, safe_member, sha, source_locator,
)


def rotation_proof(source, candidate, selected, facing):
    expected = expected_rotation_bytes(source, selected, facing)
    require(candidate["data"] == expected, "complete native differs from the independent last absolute facing oracle")
    require(candidate["count"] == source["count"] and len(candidate["data"]) == len(source["data"]),
            "rotation changed native record count or byte length")
    offset = selected["start"] + 20
    changed = [index for index, (old, new) in enumerate(zip(source["data"], candidate["data"])) if old != new]
    require(changed and all(offset <= index < offset + 4 for index in changed),
            "rotation changed bytes outside the selected four-byte facing slot")
    require(candidate["data"][:16] == source["data"][:16] and
            candidate["data"][offset:offset + 4] == struct.pack("<f", f32(facing)),
            "rotation header or exact final requested float bits differ")
    for before, after in zip(source["rows"], candidate["rows"]):
        require(before["index"] == after["index"] and before["row_key"] == after["row_key"] and
                before["creation_id"] == after["creation_id"] and before["rawcode"] == after["rawcode"],
                "rotation changed original placement identity or order")
        if before["index"] == selected["index"]:
            require(before["bytes"][:20] == after["bytes"][:20] and before["bytes"][24:] == after["bytes"][24:],
                    "selected record has a change outside facing, including nested data")
        else:
            require(before["bytes"] == after["bytes"], "rotation changed another complete original record")
    return {"source_member_sha256": source["sha256"], "candidate_member_sha256": candidate["sha256"],
            "source_index": str(selected["index"]), "source_row_key": selected["row_key"],
            "source_record_sha256": hashlib.sha256(selected["bytes"]).hexdigest(),
            "record_count": source["count"], "native_bytes": len(source["data"]),
            "source_facing_offset": offset, "changed_byte_offsets": changed,
            "requested_facing_radians": f32(facing), "requested_facing_raw": raw_f32(f32(facing)),
            "all_other_original_record_slices_exact": True, "selected_record_except_four_facing_bytes_exact": True,
            "complete_native_equals_independent_last_absolute_plan": True, "cpp_placement_algorithm_used": False}


def apply_rotations(server, evidence, original, native, selected):
    envelope = begin(server)
    rows, reply = query_entities(server, "units")
    require(rows == original["units"]["rows"] and reply["member"] == original["units"]["reply"]["member"],
            "explicit begin did not preserve the original entire unit table")
    commands, first_request = [], None
    for facing in (1.125, 1.5):
        before_rows, before_reply = rows, reply
        request = placement_request(reply, rows[selected["index"]], "rotate", facing)
        if first_request is None:
            first_request = copy.deepcopy(request)
        edited = server.tool("entities_rotate", request)
        rows, reply = query_entities(server, "units")
        expected = read_units_bytes(expected_rotation_bytes(native, selected, facing))
        check_unit_query(rows, expected)
        require(rows == expected_rotation_rows(original["units"]["rows"], selected["index"], facing) and
                reply["member"]["member_sha256"] == expected["sha256"] and server.envelope == envelope and
                edited.get("requested_facing_radians") == f32(facing) and
                source_locator(rows[selected["index"]]) == source_locator(original["units"]["rows"][selected["index"]]),
                "absolute rotation lost its original row/last float/transaction or changed other typed fields")
        command = assert_receipt(edited, "rotate", before_reply, before_rows[selected["index"]], reply, rows[selected["index"]])
        commands.append(command)
        proof = rotation_proof(native, expected, selected, facing)
        evidence.artifact(f"rotation-{len(commands)}.actual-http.json", {
            "request": request, "write": edited, "rows": rows, "reply": reply, "independent_native_plan": proof})
        evidence.check(f"absolute-rotation-{len(commands)}-complete-typed-table-cas-and-exact-f32-proof", {
            "command": command, "transaction": envelope, "proof": proof})
    require((commands[0]["session_identity"], commands[0]["command_identity"]) !=
            (commands[1]["session_identity"], commands[1]["command_identity"]),
            "two rotations reused one actual command receipt tuple")
    reject_unchanged(server, evidence, "stale-first-rotation-refused-last-full-overlay-preserved", "entities_rotate",
                     first_request, "ENTITY_REVISION_CONFLICT", rows, reply)
    for kind in ("doodads", "regions", "cameras"):
        current, described = query_entities(server, kind)
        require(current == original[kind]["rows"] and described["member"] == original[kind]["reply"]["member"],
                "two rotations changed another actual native entity table")
    return expected, rows, reply, commands


def save_rotation(args, evidence, server, source_before, native, expected):
    preview = server.tool("preview_edit", server.envelope)
    evidence.artifact("rotation.transaction.preview.json", preview)
    patches = preview.get("changes")
    require(isinstance(patches, list) and len(patches) == 1 and patches[0].get("kind") == "map-data" and
            normalized_member(patches[0].get("path", "")) == UNIT_MEMBER and
            patches[0].get("before_sha256") == native["sha256"] and patches[0].get("after_sha256") == expected["sha256"] and
            patches[0]["before_sha256"] != patches[0]["after_sha256"],
            "rotation preview is not one cumulative original-to-last-facing native patch")
    validated = server.tool("validate_edit", server.envelope)
    require(validated.get("satisfied") is True and validated.get("highest_proven_level", -1) >= 0,
            "the current final rotation generation did not validate")
    evidence.check("original-preview-and-validation-bound-to-cumulative-last-facing-native-plan", {
        "preview": preview, "validation": validated, "envelope": server.envelope})
    candidate = args.output / ("mcp-placements-c2-rotation-candidate" + args.source.suffix.lower())
    require(not candidate.exists(), "rotation candidate already exists")
    envelope = copy.deepcopy(server.envelope)
    saved = server.tool("save_map_edit_candidate", {"source_map_path": str(args.source), "output_path": str(candidate),
        "expected_source_sha256": source_before, **envelope, "script_target": "source"}, timeout=args.save_timeout)
    require(candidate.is_file() and saved.get("candidate_kind") == "mpq-map-archive" and
            saved.get("source_sha256") == source_before and saved.get("candidate_sha256") == sha(candidate) and
            saved.get("script_rebuilt") is True and saved.get("native_editor_verified") is False and
            saved.get("game_runtime_verified") is False and isinstance(saved.get("output_path"), str) and
            Path(saved["output_path"]).resolve(strict=True) == candidate and server.envelope == envelope,
            "rotation save archive identity, original transaction or evidence claims are invalid")
    for key, count in (("applied_placement_count", 1), ("applied_field_edit_count", 0), ("applied_terrain_stroke_count", 0)):
        require(type(saved.get(key)) is int and saved[key] == count, "save did not consume one cumulative placement-only replay")
    edited = saved.get("edited_members")
    require(isinstance(edited, list) and len(edited) == 1 and normalized_member(edited[0].get("name", "")) == UNIT_MEMBER and
            edited[0].get("original_sha256") == native["sha256"] and edited[0].get("saved_sha256") == expected["sha256"],
            "rotation saved-member receipt differs from the independent final native patch")
    # 现有生产 JSON 不报告物编操作数量；实际零净变化由下方七类成员全 presence/SHA 证明。
    if "applied_object_transaction_count" in saved:
        require(type(saved["applied_object_transaction_count"]) is int and saved["applied_object_transaction_count"] == 0,
                "rotation save unexpectedly reports an object transaction")
    trigger = saved.get("trigger_transaction_receipt")
    if trigger is not None:
        require(isinstance(trigger, dict) and trigger.get("transaction_id") == envelope["transaction_id"] and
                trigger.get("transaction_active") is True and trigger.get("workspace_rebound") is False,
                "rotation save changed the original transaction identity or rebound the workspace")
    evidence.receipt.update({"saved": saved, "candidate": str(candidate),
        "reported_applied_object_transaction_count": saved.get("applied_object_transaction_count")})
    evidence.check("original-save-service-real-single-cumulative-rotation-candidate", saved)
    return candidate


def reopen_candidate(args, env, evidence, servers, server, source_before, source_audit,
                     original, native, expected, selected, after_rows, candidate):
    members = list_members(args, env, evidence, candidate, "candidate")
    workspace = args.output / "candidate-workspace"
    manifest = extract_members(args, env, evidence, candidate, list(members.values()), workspace, "candidate")
    require(UNIT_MEMBER in manifest and manifest[UNIT_MEMBER]["sha256"] == expected["sha256"],
            "CLI-extracted saved units differs from the exact last-facing native plan")
    protected = {}
    for name in PROTECTED_MEMBERS:
        before, after = source_audit.get(name), manifest.get(name)
        require((before is None) == (after is None) and (before is None or before["sha256"] == after["sha256"]),
                "protected original member presence or bytes changed: " + name)
        protected[name] = {"present": before is not None, "source_sha256": before["sha256"] if before else None,
                           "candidate_sha256": after["sha256"] if after else None, "body_inspected": False}
    evidence.receipt["object_native_net_change_count"] = 0
    evidence.check("wtg-wct-seven-object-natives-other-native-and-tiara-presence-sha-preserved", {
        "members": protected, "object_native_members_checked": list(OBJECT_MEMBERS), "object_native_net_change_count": 0})
    actual = read_units(workspace / safe_member(members[UNIT_MEMBER]))
    proof = rotation_proof(native, actual, selected, 1.5)
    evidence.artifact("rotation.candidate.independent-complete-native-proof.json", proof)
    evidence.check("entire-saved-native-only-original-row-four-facing-bytes-last-requested-1-5", proof)
    reopened = PlacementHttpServer(args, env, workspace, "candidate", evidence)
    servers.append(reopened)
    reopened.start()
    rows, reply = query_entities(reopened, "units")
    check_unit_query(rows, actual)
    source_identity = context(original["units"]["reply"])["project_identity"]
    candidate_identity = context(reply)["project_identity"]
    require(rows == after_rows and reply["member"]["member_sha256"] == manifest[UNIT_MEMBER]["sha256"] and
            candidate_identity != source_identity and reopened.envelope is None and
            reply["binding"]["source_sha256"] == reopened.record["initial_revision"]["revision"]["source_sha256"] and
            reply["binding"]["source_sha256"] != original["units"]["reply"]["binding"]["source_sha256"],
            "new owned MCP service did not reopen the exact entire final table with its own source identity")
    for kind in ("doodads", "regions", "cameras"):
        current, described = query_entities(reopened, kind)
        require(current == original[kind]["rows"] and described["member"] == original[kind]["reply"]["member"],
                "saved rotation candidate lost or changed another source entity table")
    require(reopened.record["initial_revision"]["revision"]["ui_catalog_sha256"] ==
            server.record["initial_revision"]["revision"]["ui_catalog_sha256"],
            "candidate was reopened against a different actual catalog")
    evidence.artifact("rotation.candidate.actual-http-readback.json", {"rows": rows, "reply": reply})
    evidence.check("new-owned-mcp-complete-saved-rotation-and-other-source-tables-reopened", {
        "source_project_identity": source_identity, "candidate_project_identity": candidate_identity,
        "source_archive_sha256": source_before, "candidate_archive_sha256": sha(candidate),
        "candidate_directory_aggregate_sha256": reply["binding"]["source_sha256"],
        "native_member_sha256": manifest[UNIT_MEMBER]["sha256"], "selected_row": source_locator(rows[selected["index"]]),
        "same_actual_catalog": True, "source_locators_requeried": True, "permanent_ID_claimed": False})
    reopened.close()


def main():
    args = inputs()  # 与冻结 C2 探针保持相同完整 CLI 参数及输入边界。
    evidence = Evidence(args.output)
    evidence.receipt.update({"schema": "audastra-mcp-rotation-save-c2-http-probe-v1",
        "scope": "same-original-unit-two-absolute-rotations-original-save-cli-full-native-new-mcp-reopen",
        "whole_C_group_verified": False, "row_locators_across_revisions_claimed_permanent": False,
        "saved_rotation_verified": False,
        "arguments": {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()}})
    servers, source_before, workspace_before = [], None, None
    try:
        env, source_before, workspace_before = prepare(args, evidence)
        members = list_members(args, env, evidence, args.source, "source")
        require({UNIT_MEMBER, "war3map.wtg", "war3map.wct"} <= members.keys(), "source lacks unit/trigger native members")
        names = [members[name] for name in (UNIT_MEMBER,) + PROTECTED_MEMBERS if name in members]
        audit_dir = args.output / "source-member-hashes"
        audit = extract_members(args, env, evidence, args.source, names, audit_dir, "source-audit")
        for name, bound in audit.items():
            path = args.workspace / safe_member(members[name])
            require(path.is_file() and sha(path) == bound["sha256"], "input workspace/source member mismatch: " + name)
        native = read_units(audit_dir / safe_member(members[UNIT_MEMBER]))
        selected = next((row for row in native["rows"] if row["rawcode"] != b"sloc" and 0 <= row["owner"] <= 15 and
                         all(abs(value) < 900000 for value in row["position"]) and
                         all(abs(value) <= 1000 for value in row["scale"]) and row["facing"] not in (1.125, 1.5)), None)
        require(selected is not None, "source has no safe existing non-start unit with two distinct requested facings")
        server = PlacementHttpServer(args, env, args.workspace, "source", evidence)
        servers.append(server)
        server.start()
        original = {}
        for kind in ENTITY_MEMBERS:
            rows, reply = query_entities(server, kind)
            original[kind] = {"rows": rows, "reply": reply}
            member = reply["member"]
            require((ENTITY_MEMBERS[kind] in members) == (member["state"] == "ready") and
                    (member["state"] == "missing" or member["member_sha256"] == audit[ENTITY_MEMBERS[kind]]["sha256"]),
                    "actual source HTTP member presence/SHA differs from independent archive extraction")
            evidence.artifact(f"source-{kind}.before.json", original[kind])
        check_unit_query(original["units"]["rows"], native)
        require(original["units"]["reply"]["binding"]["source_sha256"] ==
                server.record["initial_revision"]["revision"]["source_sha256"], "source directory aggregate binding is invalid")
        evidence.check("complete-original-unit-native-wire-and-real-http-source-binding", {
            "unit_count": native["count"], "nested_record_count": native["nested_records"],
            "native_member_sha256": native["sha256"], "source_archive_sha256": source_before,
            "source_directory_aggregate_sha256": original["units"]["reply"]["binding"]["source_sha256"],
            "selected_source_index": str(selected["index"]), "selected_row_key": selected["row_key"],
            "oracle": "independent-classic-wire-exact-four-byte-slot"})
        expected, after_rows, after_reply, commands = apply_rotations(server, evidence, original, native, selected)
        candidate = save_rotation(args, evidence, server, source_before, native, expected)
        current_rows, current_reply = query_entities(server, "units")
        require(current_rows == after_rows and current_reply == after_reply, "save changed the current rotation overlay or CAS")
        restore_check(server, evidence, original, "saved-rotation-rollback-restores-all-four-original-source-tables")
        server.close()
        reopen_candidate(args, env, evidence, servers, server, source_before, audit,
                         original, native, expected, selected, after_rows, candidate)
        require(sha(args.source) == source_before and inventory(args.workspace) == workspace_before and
                not evidence.receipt["cleanup_errors"] and all(item.process is not None and item.process.poll() is not None for item in servers),
                "original source/workspace identity or actual owned-server cleanup failed")
        evidence.receipt["rotation"] = {"two_absolute_commands_verified": True, "commands": commands,
            "last_requested_radians": 1.5, "last_native_facing_raw": raw_f32(1.5),
            "complete_native_four_byte_only_verified": True, "rollback_verified": True,
            "candidate_saved": True, "new_owned_mcp_reopened": True}
        evidence.receipt["saved_rotation_verified"] = True
        evidence.check("all-owned-source-and-candidate-http-processes-actually-cleaned", {
            "owned_process_count": len(servers), "all_exited": True,
            "exit_codes": [item.process.returncode for item in servers]})
        evidence.receipt["passed"] = True
        evidence.checkpoint("passed-independent-c2-rotation-save-and-complete-native-reopen")
    except Exception as error:
        evidence.receipt["failed_stage"] = evidence.receipt["stage"]
        evidence.receipt["error"] = evidence.scrub(str(error))
        evidence.checkpoint("failed")
    finally:
        for server in reversed(servers):
            try:
                server.close()
            except Exception as error:
                evidence.receipt["cleanup_errors"].append({"server": server.label, "close_error": evidence.scrub(str(error))})
        try:
            if source_before is not None:
                source_after = sha(args.source)
                evidence.receipt.update({"source_sha256_after": source_after, "source_unchanged": source_after == source_before})
            if workspace_before is not None:
                current = inventory(args.workspace)
                evidence.artifact("source-workspace.after.json", current)
                evidence.receipt.update({"workspace_inventory_sha256_after": json_sha(current), "source_workspace_unchanged": current == workspace_before})
            if any(item.process is not None and item.process.poll() is None for item in servers):
                evidence.receipt["cleanup_errors"].append({"owned_server_still_running": True})
        except Exception as error:
            evidence.receipt["cleanup_errors"].append({"identity_recheck_error": evidence.scrub(str(error))})
        if (evidence.receipt["cleanup_errors"] or evidence.receipt.get("source_unchanged") is False or
                evidence.receipt.get("source_workspace_unchanged") is False):
            evidence.receipt["passed"] = False
        if not evidence.receipt["passed"]:
            evidence.receipt["saved_rotation_verified"] = False
        evidence.checkpoint()
        evidence.log.close()
    print(json.dumps({"passed": evidence.receipt["passed"], "receipt": str(args.output / "receipt.json"),
                      "candidate": evidence.receipt.get("candidate"), "error": evidence.receipt.get("error")}, ensure_ascii=False))
    return 0 if evidence.receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
