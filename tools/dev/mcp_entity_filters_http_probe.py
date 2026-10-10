"""C3 units 精确筛选、原索引游标及已签收 C2 候选的独立 HTTP 查询探针。

仅新建证据目录；不生成新保存候选。源/工作区保持，凭据和进程沿用冻结 helper。
实际服务由运行此脚本的宿主负责，本脚本不提供 GUI、native Save 或游戏证明。
"""

import argparse
import collections
import copy
import json
from pathlib import Path
import struct

from mcp_object_http_probe import Evidence, extract_members, inventory, json_sha, list_members, require, safe_member, sha
from mcp_variables_http_probe import context
from mcp_entities_http_probe import HEX_SHA, change, prepare, query_entities, unsigned
from mcp_placements_http_probe import (
    UNIT_MEMBER, PROTECTED_MEMBERS, PlacementHttpServer, begin, check_unit_query,
    copy_proof, read_units, read_units_bytes,
)
from mcp_rotation_save_http_probe import rotation_proof


SCAN_LIMIT = 4096


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def canonical_filter(value):
    require(isinstance(value, dict) and set(value) <= {"type_id", "player"}, "invalid independent filter")
    result = {}
    if "type_id" in value:
        text = value["type_id"]
        require(isinstance(text, str), "independent type_id must be text")
        raw = bytes.fromhex(text[4:]) if len(text) == 12 and text.startswith("hex:") else text.encode("utf-8")
        require(len(raw) == 4, "independent type_id must encode exactly four bytes")
        result["type_id"] = "hex:" + raw.hex()
    if "player" in value:
        require(type(value["player"]) is int and -(2**31) <= value["player"] < 2**31, "independent player is not int32")
        result["player"] = value["player"]
    return result


def native_matches(native, value):
    value = canonical_filter(value)
    raw = bytes.fromhex(value["type_id"][4:]) if "type_id" in value else None
    return [row["index"] for row in native["rows"] if
            (raw is None or row["rawcode"] == raw) and ("player" not in value or row["owner"] == value["player"])]


class FilterHttpServer(PlacementHttpServer):
    def start(self):
        super().start()
        tools = load(self.evidence.output / f"{self.label}.tools-list.json")["tools"]
        require(len(tools) == len({tool["name"] for tool in tools}) == 42, "C3 must retain exactly 42 unique tool names")
        schema = next(tool["inputSchema"] for tool in tools if tool["name"] == "entities_query")
        props = schema["properties"]
        filter_schema = props.get("filter", {})
        require(filter_schema.get("type") == "object" and filter_schema.get("additionalProperties") is False and
                set(filter_schema.get("properties", {})) == {"type_id", "player"} and
                filter_schema["properties"]["type_id"].get("type") == "string" and
                filter_schema["properties"]["player"].get("type") == "integer", "C3 filter schema differs")
        cursor_schema = props["page"]["properties"].get("cursor", {})
        require(set(cursor_schema.get("required", [])) == {"binding", "kind", "member_sha256", "filter", "next_index"}
                and cursor_schema.get("properties", {}).get("kind", {}).get("enum") == ["units"], "C3 cursor schema differs")
        self.evidence.check(f"{self.label}-actual-c3-filter-schema-and-42-tools", {
            "count": 42, "query_schema_sha256": json_sha(schema), "no_new_tool_names": True})
        return self


def filtered(server, evidence, label, native, full_rows, full_reply, value, limit, start=0, equivalent=None):
    require(native["count"] <= SCAN_LIMIT, "this real-map probe requires complete bounded small-table totals")
    expected_indices = native_matches(native, value)
    wanted = [index for index in expected_indices if index >= start]
    result, pages, cursor, next_index = [], [], None, start
    canonical = canonical_filter(value)
    envelope = copy.deepcopy(server.envelope)
    for page_number in range(native["count"] + 1):
        top = equivalent if cursor is not None and equivalent is not None else value
        request = {"kind": "units", **context(full_reply), "expected_member_sha256": native["sha256"], "filter": top,
                   "page": {"limit": limit, **({"cursor": cursor} if cursor is not None else {"start_index": str(start)})}}
        reply = server.tool("entities_query", request)
        require(context(reply) == context(full_reply) and reply["binding"] == full_reply["binding"] and
                reply["member"] == full_reply["member"] and server.envelope == envelope,
                "filtered page crossed native/CAS/binding/original transaction")
        require(reply.get("filter") == canonical and reply.get("matched_total_complete") is True and
                unsigned(reply.get("matched_total"), "matched_total") == len(expected_indices) and
                unsigned(reply.get("examined"), "examined") == native["count"] <= SCAN_LIMIT and
                reply.get("scan_budget_exhausted") is False, "filtered page fabricated totals or exceeded its exact scan budget")
        rows = reply.get("rows")
        expected_page = [index for index in expected_indices if index >= next_index][:limit]
        require(isinstance(rows, list) and rows == [full_rows[index] for index in expected_page],
                "filtered DTOs/native predicate/source indices differ from the independent oracle")
        following = reply.get("next_cursor")
        require(reply.get("truncated") is (following is not None), "filtered truncated/cursor disagree")
        remaining = [index for index in expected_indices if expected_page and index > expected_page[-1]]
        if following is None:
            require(not remaining and result + rows == [full_rows[index] for index in wanted], "filtered page ended before all matches")
        else:
            require(set(following) == {"binding", "kind", "member_sha256", "filter", "next_index"} and
                    following["binding"] == reply["binding"] and following["kind"] == "units" and
                    following["member_sha256"] == native["sha256"] and following["filter"] == canonical,
                    "filtered cursor is not bound to the exact member/filter/revision")
            following_index = unsigned(following["next_index"], "next_index")
            require(expected_page and remaining and expected_page[-1] < following_index <= remaining[0],
                    "filtered cursor repeated or skipped a native match")
            next_index = following_index
        pages.append({"request": request, "reply": reply})
        result.extend(rows)
        if following is None:
            evidence.artifact(label + ".pages.json", pages)
            evidence.check(label, {"filter": canonical, "limit": limit, "start_index": str(start),
                "matched_total": str(len(expected_indices)), "returned": len(result), "pages": len(pages),
                "source_indices": [row["source_index"] for row in result], "full_dtos_exact": True,
                "each_examined": native["count"], "scan_budget": SCAN_LIMIT})
            return pages
        cursor = following
    raise RuntimeError("filtered pagination exceeded the independent original-row bound")


def choose_filter(native):
    groups = collections.Counter((row["rawcode"], row["owner"]) for row in native["rows"]
                                 if row["rawcode"] != b"sloc" and 0 <= row["owner"] <= 15)
    for (raw, player), count in groups.most_common():
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            continue
        if count >= 2 and any(char.isalpha() for char in text) and len(text.swapcase().encode("utf-8")) == 4:
            return {"type_id": text, "player": player}
    raise RuntimeError("source has no real repeated UTF-8 type/player filter fixture")


def exercise_queries(server, evidence, native, rows, reply, prefix):
    chosen = choose_filter(native)
    canonical = canonical_filter(chosen)
    one = None
    for limit in (1, 7, 128):
        pages = filtered(server, evidence, f"{prefix}-combined-filter-page-{limit}", native, rows, reply, chosen, limit,
                         equivalent=canonical)
        if limit == 1:
            one = pages[0]
    cases = [("empty-filter", {}), ("type-only", {"type_id": chosen["type_id"]}),
             ("uppercase-hex-equivalent", {"type_id": "hex:" + chosen["type_id"].encode("utf-8").hex().upper()}),
             ("four-byte-hex-literal", {"type_id": "hex:"}),
             ("player-only", {"player": chosen["player"]}),
             ("case-sensitive-variant", {"type_id": chosen["type_id"].swapcase()}),
             ("four-byte-utf8", {"type_id": "雪x"}), ("lossless-non-utf8-hex", {"type_id": "hex:ff00ff00"}),
             ("int32-min-no-match", {"player": -(2**31)}), ("int32-max-no-match", {"player": 2**31 - 1})]
    unusual = next((row["owner"] for row in native["rows"] if row["owner"] > 15), None)
    if unusual is not None:
        cases.append(("native-owner-outside-write-range", {"player": unusual}))
    for label, value in cases:
        filtered(server, evidence, f"{prefix}-{label}", native, rows, reply, value, 128)
    indices = native_matches(native, chosen)
    filtered(server, evidence, f"{prefix}-explicit-original-start-index", native, rows, reply, chosen, 7, start=indices[1])
    require(one["reply"]["next_cursor"] is not None, "actual multi-match filter produced no continuation")
    return chosen, one["reply"]["next_cursor"]


def reject_unchanged(server, evidence, label, request, rows, reply, archive, native_path, code=None, message=None):
    envelope = copy.deepcopy(server.envelope)
    revision = server.tool("get_revision", {})
    before_hashes = {"archive": sha(archive), "native": sha(native_path)}
    if message is not None:
        rejected = server.request("tools/call", {"name": "entities_query", "arguments": request},
                                  expected_rpc_error={"code": -32602, "message": message})
        proof = {"route": "schema-jsonrpc", "rpc_response": rejected, "http_200_same_id_verified": True}
    else:
        rejected = server.rejection("entities_query", request, code)
        proof = {"route": "domain-tool-receipt", "expected_code": code, "receipt": rejected}
    require(query_entities(server, "units") == (rows, reply) and server.tool("get_revision", {}) == revision and
            server.envelope == envelope and {"archive": sha(archive), "native": sha(native_path)} == before_hashes,
            "filter refusal changed units/native hashes/revision/original transaction")
    proof.update({"entire_unit_table_and_binding_unchanged": True, "revision_and_transaction_unchanged": True,
                  "source_archive_and_native_unchanged": True})
    evidence.artifact(label + ".refusal.json", {"request": request, "proof": proof})
    evidence.check(label, proof)


def exercise_refusals(server, evidence, native, rows, reply, archive, native_path, chosen, cursor):
    request = {"kind": "units", "filter": chosen, **context(reply), "expected_member_sha256": native["sha256"]}
    cases = [({"filter": []}, None, "arguments.filter must be an object"),
             ({"filter": {"type_id": 1}}, None, "arguments.filter.type_id must be a string"),
             ({"filter": {"player": "12"}}, None, "arguments.filter.player must be an integer"),
             ({"filter": {"player": 1.5}}, None, "arguments.filter.player must be an integer"),
             ({"filter": {"player": True}}, None, "arguments.filter.player must be an integer"),
             ({"filter": {"foo": 1}}, None, "arguments.filter contains unknown property 'foo'"),
             ({"filter": {"type_id": "abc"}}, "ENTITY_FILTER_TYPE_ID", None),
             ({"filter": {"type_id": "hex:zzzzzzzz"}}, "ENTITY_FILTER_TYPE_ID", None),
             ({"filter": {"type_id": "a" * 13}}, "ENTITY_ARGUMENT_BUDGET", None),
             ({"filter": {"player": 2**31}}, "ENTITY_FILTER_PLAYER", None),
             ({"kind": "regions", "filter": {}}, "ENTITY_FILTER_KIND", None)]
    for number, (patch, code, message) in enumerate(cases, 1):
        bad = {**request, **patch}
        if bad["kind"] != "units":
            bad.pop("expected_member_sha256")
        reject_unchanged(server, evidence, f"filter-refusal-{number:02}", bad, rows, reply, archive, native_path, code, message)
    cursor_request = {**request, "page": {"limit": 1, "cursor": cursor}}
    bad = copy.deepcopy(cursor_request); bad["filter"]["player"] = (chosen["player"] + 1) % 16
    reject_unchanged(server, evidence, "cursor-filter-mismatch", bad, rows, reply, archive, native_path, "ENTITY_QUERY_CURSOR_STALE")
    bad = copy.deepcopy(cursor_request); bad["page"]["cursor"]["member_sha256"] = "0" * 64
    reject_unchanged(server, evidence, "cursor-member-sha-mismatch", bad, rows, reply, archive, native_path, "ENTITY_QUERY_CURSOR_STALE")
    bad = copy.deepcopy(cursor_request); bad["page"]["cursor"]["binding"]["revision"] = str(int(cursor["binding"]["revision"]) + 1)
    reject_unchanged(server, evidence, "cursor-revision-mismatch", bad, rows, reply, archive, native_path, "ENTITY_QUERY_CURSOR_STALE")
    bad = copy.deepcopy(cursor_request); bad["page"]["start_index"] = "0"
    reject_unchanged(server, evidence, "cursor-plus-start-index", bad, rows, reply, archive, native_path, "ENTITY_QUERY_CURSOR_INVALID")
    bad = copy.deepcopy(cursor_request); bad["page"]["cursor"]["next_index"] = "00"
    reject_unchanged(server, evidence, "cursor-noncanonical-index-schema", bad, rows, reply, archive, native_path,
                     message="arguments.page.cursor.next_index does not match its required format")
    bad = copy.deepcopy(cursor_request); del bad["page"]["cursor"]["member_sha256"]
    reject_unchanged(server, evidence, "cursor-missing-member-sha-schema", bad, rows, reply, archive, native_path,
                     message="arguments.page.cursor is missing required property 'member_sha256'")


def exercise_overlay(server, evidence, native, rows, reply, archive, native_path, chosen):
    envelope = begin(server)
    active_rows, active_reply = query_entities(server, "units")
    pages = filtered(server, evidence, "active-before-owner-filter", native, active_rows, active_reply, chosen, 1)
    old_cursor = pages[0]["reply"]["next_cursor"]
    index = native_matches(native, chosen)[0]
    owner = (native["rows"][index]["owner"] + 1) % 16
    edited = server.tool("entities_set_fields", {**context(active_reply), "kind": "units", "expected_member_sha256": native["sha256"],
        "changes": [change(active_rows[index], "owner", owner)]})
    expected = bytearray(native["data"]); struct.pack_into("<i", expected, native["rows"][index]["start"] + 37, owner)
    overlay = read_units_bytes(bytes(expected))
    new_rows, new_reply = query_entities(server, "units")
    check_unit_query(new_rows, overlay)
    expected_rows = copy.deepcopy(rows); expected_rows[index]["owner"] = owner
    for field in expected_rows[index]["fields"]:
        if field["id"] == "owner": field.update({"raw": str(owner), "value": owner})
    require(new_rows == expected_rows and new_reply["member"]["member_sha256"] == overlay["sha256"] and
            context(new_reply) != context(active_reply) and server.envelope == envelope and
            unsigned(edited.get("applied_fields"), "applied_fields") == 1, "real C1 owner overlay differs from original native four-byte oracle")
    filtered(server, evidence, "active-owner-overlay-removes-original-filter-match", overlay, new_rows, new_reply, chosen, 7)
    filtered(server, evidence, "active-owner-overlay-new-player-filter", overlay, new_rows, new_reply,
             {**chosen, "player": owner}, 7)
    stale = {"kind": "units", **context(new_reply), "expected_member_sha256": overlay["sha256"], "filter": chosen,
             "page": {"limit": 1, "cursor": old_cursor}}
    reject_unchanged(server, evidence, "old-cursor-after-real-owner-overlay", stale, new_rows, new_reply,
                     archive, native_path, "ENTITY_QUERY_CURSOR_STALE")
    server.rollback()
    restored_rows, restored_reply = query_entities(server, "units")
    require(restored_rows == rows and restored_reply["member"] == reply["member"] and server.envelope is None,
            "original rollback did not restore all unit DTOs/member SHA")
    check_unit_query(restored_rows, native)
    filtered(server, evidence, "rollback-original-filter-results-restored", native, restored_rows, restored_reply, chosen, 128)
    evidence.check("actual-owner-overlay-native-cas-filter-and-original-rollback", {
        "source_index": str(index), "before_owner": chosen["player"], "after_owner": owner,
        "original_native_sha256": native["sha256"], "overlay_native_sha256": overlay["sha256"], "saved": False})


def accepted_candidate(receipt_path, expected_sha, operation, evidence):
    receipt_path = receipt_path.resolve(strict=True)
    receipt = load(receipt_path)
    require(receipt.get("passed") is True and receipt.get("source_unchanged") is True and
            receipt.get("source_workspace_unchanged") is True and receipt.get("cleanup_errors") == [] and
            receipt.get("native_editor_verified") is False and receipt.get("game_runtime_verified") is False,
            "candidate provenance receipt was not accepted with bounded source/cleanup evidence")
    require(receipt.get("saved_rotation_verified") is (operation == "rotation"), "wrong accepted C2 operation")
    archive = Path(receipt["candidate"]).resolve(strict=True)
    require(archive.is_file() and sha(archive) == expected_sha == receipt["saved"]["candidate_sha256"], "accepted candidate archive SHA drift")
    manifest = load(receipt_path.parent / "candidate.manifest.json")
    workspace = Path(manifest["workspace"]).resolve(strict=True)
    require(workspace.is_dir() and Path(manifest["archive"]).resolve(strict=True) == archive, "candidate manifest archive/workspace mismatch")
    members = {row["member"].replace("\\", "/").casefold(): row for row in manifest["members"]}
    require(len(members) == len(manifest["members"]) == manifest["member_count"], "ambiguous accepted candidate members")
    for row in members.values():
        member_path = workspace / safe_member(row["member"])
        require(member_path.is_file() and member_path.stat().st_size == row["bytes"] and sha(member_path) == row["sha256"],
                "accepted candidate extracted member changed")
    native = read_units(workspace / safe_member(members[UNIT_MEMBER]["member"]))
    prior = load(receipt_path.parent / "source-audit.manifest.json")
    original_members = {row["member"].replace("\\", "/").casefold(): row for row in prior["members"]}
    original_names = {name.replace("\\", "/").casefold() for name in load(receipt_path.parent / "source.member-list.json")["names"]}
    for name in PROTECTED_MEMBERS:
        require((name in original_names) == (name in original_members) == (name in members) and
                (name not in original_members or original_members[name]["sha256"] == members[name]["sha256"]),
                "accepted C2 protected member presence/SHA no longer matches its original audit")
    prior_unit = next(row for row in prior["members"] if row["member"].casefold() == UNIT_MEMBER)
    prior_path = Path(prior["workspace"]) / safe_member(prior_unit["member"])
    require(sha(prior_path) == prior_unit["sha256"], "accepted original unit oracle source drift")
    original = read_units(prior_path)
    if operation == "copy":
        proof = load(receipt_path.parent / "copy.requested.independent-native-plan.json")
        require(copy_proof(original, native, original["rows"][int(proof["source_index"])],
                           tuple(proof["offset"][axis] for axis in ("x", "y", "z"))) == proof, "accepted copy lost independent whole-wire proof")
        previous = load(receipt_path.parent / "copy.candidate.actual-http-readback.json")
    else:
        proof = load(receipt_path.parent / "rotation.candidate.independent-complete-native-proof.json")
        require(rotation_proof(original, native, original["rows"][int(proof["source_index"])], 1.5) == proof,
                "accepted rotation lost independent whole-wire proof")
        previous = load(receipt_path.parent / "rotation.candidate.actual-http-readback.json")
    check_unit_query(previous["rows"], native)
    evidence.check(f"accepted-{operation}-archive-and-entire-native-provenance-bound", {
        "prior_receipt_sha256": sha(receipt_path), "archive_sha256": expected_sha, "native_sha256": native["sha256"],
        "rows": native["count"], "original_c2_byte_proof_recomputed": True, "new_save_attempted": False})
    return {"label": operation, "archive": archive, "workspace": workspace, "native": native,
            "previous_rows": previous["rows"], "archive_sha256": expected_sha, "inventory": inventory(workspace),
            "source_archive_sha256": receipt["source_sha256_before"],
            "tool_names": {tool["name"] for tool in load(receipt_path.parent / "source.tools-list.json")["tools"]}}


def inputs():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("server", "cli", "source", "workspace", "ydwe-root", "war3-root", "output", "copy-receipt", "rotation-receipt"):
        parser.add_argument("--" + name, required=True, type=Path)
    for name in ("source", "copy", "rotation"):
        parser.add_argument("--expected-" + name + "-sha256", required=True)
    parser.add_argument("--qt-bin", type=Path)
    for name, default in (("start", 60), ("request", 60), ("cli", 60)):
        parser.add_argument("--" + name + "-timeout", type=float, default=default)
    args = parser.parse_args()
    for name in ("source", "copy", "rotation"):
        require(HEX_SHA.fullmatch(getattr(args, "expected_" + name + "_sha256")), "expected archive SHA must be lowercase hex")
    for key in ("start_timeout", "request_timeout", "cli_timeout"):
        require(0 < getattr(args, key) <= 3600, "invalid timeout")
    args.output = args.output.resolve()
    forbidden = [args.workspace.resolve(), args.copy_receipt.resolve().parent, args.rotation_receipt.resolve().parent]
    require(not any(args.output.is_relative_to(path) for path in forbidden), "new evidence must be outside all source/accepted evidence trees")
    args.output.mkdir(parents=True, exist_ok=False)
    return args


def main():
    args = inputs(); evidence = Evidence(args.output)
    evidence.receipt.update({"schema": "audastra-mcp-entity-filter-c3-http-probe-v1",
        "scope": "units-exact-type-player-native-index-cursor-owner-overlay-and-two-accepted-c2-candidate-readonly-slices",
        "evidence_level": "independent-mcp-http-filter-and-native-byte-oracle-with-accepted-candidate-provenance",
        "whole_C_group_verified": False, "new_save_attempted": False, "candidate_saved_in_this_probe": False,
        "row_locators_across_revisions_claimed_permanent": False})
    servers, candidates, source_before, workspace_before = [], [], None, None
    try:
        env, source_before, workspace_before = prepare(args, evidence)
        for operation in ("copy", "rotation"):
            candidates.append(accepted_candidate(getattr(args, operation + "_receipt"),
                getattr(args, "expected_" + operation + "_sha256"), operation, evidence))
        require(all(candidate["source_archive_sha256"] == source_before for candidate in candidates) and
                candidates[0]["tool_names"] == candidates[1]["tool_names"] and len(candidates[0]["tool_names"]) == 42,
                "accepted C2 candidates do not share the authorized original archive/42-tool discovery")
        members = list_members(args, env, evidence, args.source, "source")
        names = [members[name] for name in (UNIT_MEMBER,) + PROTECTED_MEMBERS if name in members]
        require(UNIT_MEMBER in members, "source has no units native member")
        audit_dir = args.output / "source-member-hashes"
        audit = extract_members(args, env, evidence, args.source, names, audit_dir, "source-audit")
        for name, row in audit.items():
            require(sha(args.workspace / safe_member(members[name])) == row["sha256"], "source archive/workspace native mismatch")
        native_path = args.workspace / safe_member(members[UNIT_MEMBER]); native = read_units(native_path)
        server = FilterHttpServer(args, env, args.workspace, "source", evidence); servers.append(server); server.start()
        require({tool["name"] for tool in load(args.output / "source.tools-list.json")["tools"]} == candidates[0]["tool_names"],
                "C3 changed the accepted standalone tool names")
        rows, reply = query_entities(server, "units"); check_unit_query(rows, native)
        require(reply["member"]["member_sha256"] == native["sha256"] and server.envelope is None and
                reply["binding"]["source_sha256"] == server.record["initial_revision"]["revision"]["source_sha256"], "source units identity mismatch")
        evidence.artifact("source-units.before.json", {"rows": rows, "reply": reply})
        chosen, cursor = exercise_queries(server, evidence, native, rows, reply, "source")
        exercise_refusals(server, evidence, native, rows, reply, args.source, native_path, chosen, cursor)
        exercise_overlay(server, evidence, native, rows, reply, args.source, native_path, chosen)
        server.close()
        for candidate in candidates:
            reopened = FilterHttpServer(args, env, candidate["workspace"], "accepted-" + candidate["label"], evidence)
            servers.append(reopened); reopened.start()
            require({tool["name"] for tool in load(args.output / f"{reopened.label}.tools-list.json")["tools"]} == candidates[0]["tool_names"],
                    "accepted candidate service changed standalone tool names")
            actual_rows, actual_reply = query_entities(reopened, "units"); check_unit_query(actual_rows, candidate["native"])
            require(actual_rows == candidate["previous_rows"] and actual_reply["member"]["member_sha256"] == candidate["native"]["sha256"] and
                    context(actual_reply)["project_identity"] != context(reply)["project_identity"] and reopened.envelope is None,
                    "C3 reopened candidate lost accepted DTO/native identity or reused original project")
            exercise_queries(reopened, evidence, candidate["native"], actual_rows, actual_reply, "accepted-" + candidate["label"])
            reopened.close()
        evidence.receipt["passed"] = True
        evidence.checkpoint("passed-independent-c3-filter-original-overlay-rollback-and-accepted-c2-readonly-slices")
    except Exception as error:
        evidence.receipt["failed_stage"] = evidence.receipt["stage"]; evidence.receipt["error"] = evidence.scrub(str(error))
        evidence.checkpoint("failed")
    finally:
        for server in reversed(servers):
            try: server.close()
            except Exception as error: evidence.receipt["cleanup_errors"].append({"server": server.label, "error": evidence.scrub(str(error))})
        try:
            if source_before is not None:
                evidence.receipt.update({"source_sha256_after": sha(args.source), "source_unchanged": sha(args.source) == source_before})
            if workspace_before is not None:
                after = inventory(args.workspace); evidence.artifact("source-workspace.after.json", after)
                evidence.receipt["source_workspace_unchanged"] = after == workspace_before
            for candidate in candidates:
                require(sha(candidate["archive"]) == candidate["archive_sha256"] and inventory(candidate["workspace"]) == candidate["inventory"],
                        "readonly accepted candidate archive/workspace changed")
            evidence.receipt["accepted_candidates_unchanged"] = len(candidates) == 2
            require(all(item.process is None or item.process.poll() is not None for item in servers), "owned process remains running")
            if evidence.receipt["passed"]:
                require(len(servers) == 3 and all(item.process is not None and item.record.get("owned") is True and
                        item.record.get("owned_process_terminated") is True for item in servers), "three successful owned services were not actually cleaned")
                evidence.check("all-three-owned-http-services-cleaned-and-inputs-retained", {
                    "owned_process_count": 3, "all_exited": True, "exit_codes": [item.process.returncode for item in servers],
                    "accepted_candidate_archives_and_workspaces_unchanged": True})
        except Exception as error: evidence.receipt["cleanup_errors"].append({"identity_recheck_error": evidence.scrub(str(error))})
        if evidence.receipt["cleanup_errors"] or not evidence.receipt.get("source_unchanged") or not evidence.receipt.get("source_workspace_unchanged"):
            evidence.receipt["passed"] = False
        evidence.checkpoint(); evidence.log.close()
    print(json.dumps({"passed": evidence.receipt["passed"], "receipt": str(args.output / "receipt.json"),
                      "error": evidence.receipt.get("error")}, ensure_ascii=False))
    return 0 if evidence.receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
