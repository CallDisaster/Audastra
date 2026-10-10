"""C1 独立 HTTP 区域字段候选保存与严格 classic W3R 字节回读探针。

仅证明既有实例字段入口的一次区域 left/right 编辑，不宣称整个 C 组完成。
HTTP、CLI、来源清单、凭据脱敏和自有进程清理复用现有物编探针。
输入目录和源归档只读；候选与证据仅写全新输出目录。调用 schema 以实际服务为准。
"""

import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct

from mcp_object_http_probe import (
    Evidence, MAX_PAGES, OBJECT_MEMBERS, extract_members, inventory,
    json_sha, list_members, normalized_member, require, safe_member, sha,
)
from mcp_variables_http_probe import VariableHttpServer, context


ENTITY_TOOLS = {"entities_query", "entities_set_fields"}
ENTITY_MEMBERS = {"units": "war3mapunits.doo", "doodads": "war3map.doo",
                  "regions": "war3map.w3r", "cameras": "war3map.w3c"}
PROTECTED_MEMBERS = ("war3map.wtg", "war3map.wct", "tiara.ai", "tiara.blp",
                     "war3mapunits.doo", "war3map.doo", "war3map.w3c",
                     "war3map.w3i", "war3map.w3e", "war3map.wpm", "war3map.w3s",
                     "war3map.wts", "war3map.imp") + OBJECT_MEMBERS
MAX_MEMBER_BYTES = 64 * 1024 * 1024
MAX_RECORDS = 100000
MAX_TEXT_BYTES = 1024 * 1024
HEX_SHA = re.compile(r"[0-9a-f]{64}\Z")


class WireReader:
    """独立有界 wire reader；不调用生产解析器或生产序列化代码。"""

    def __init__(self, data):
        require(isinstance(data, bytes) and 8 <= len(data) <= MAX_MEMBER_BYTES,
                "classic auxiliary member is outside the independent byte budget")
        self.data, self.offset = data, 0

    def take(self, size):
        require(0 <= size <= len(self.data) - self.offset, "truncated classic auxiliary record")
        start = self.offset
        self.offset += size
        return self.data[start:self.offset]

    def text(self):
        end = self.data.find(b"\0", self.offset, min(len(self.data), self.offset + MAX_TEXT_BYTES + 1))
        require(end >= self.offset, "unterminated or oversized classic auxiliary string")
        raw = self.take(end - self.offset)
        self.take(1)
        return raw

    def header(self, version, minimum):
        actual, count = struct.unpack("<II", self.take(8))
        require(actual == version, "unsupported classic auxiliary version")
        require(count <= MAX_RECORDS and count <= (len(self.data) - 8) // minimum,
                "classic auxiliary count exceeds the record/remaining-byte budget")
        return count


def read_regions_bytes(data):
    reader = WireReader(data)
    count = reader.header(5, 30)
    rows = []
    for index in range(count):
        start = reader.offset
        bounds_bytes = reader.take(16)
        bounds = dict(zip(("left", "bottom", "right", "top"), struct.unpack("<4f", bounds_bytes)))
        require(all(math.isfinite(value) for value in bounds.values()), "non-finite W3R bounds")
        name = reader.text()
        creation = struct.unpack("<i", reader.take(4))[0]
        reader.take(4)  # weather rawcode，原样保留。
        reader.text()   # ambient sound raw bytes，原样保留。
        reader.take(4)  # BGRA，原样保留。
        rows.append({"index": index, "start": start, "end": reader.offset,
                     "bounds": bounds, "name_hex": name.hex(), "creation_number": creation,
                     "row_key": "region:" + str(creation) + ":" + name.hex()})
    require(reader.offset == len(data), "W3R has an unknown trailing layout; independent classic proof unavailable")
    return {"data": data, "sha256": hashlib.sha256(data).hexdigest(), "rows": rows, "count": count}


def read_cameras_bytes(data):
    reader = WireReader(data)
    count = reader.header(0, 41)
    rows = []
    for index in range(count):
        start = reader.offset
        values = struct.unpack("<10f", reader.take(40))
        require(all(math.isfinite(value) for value in values), "non-finite classic W3C field")
        name = reader.text()
        rows.append({"index": index, "start": start, "end": reader.offset,
                     "name_hex": name.hex(), "row_key": "camera:" + name.hex(),
                     "values": dict(zip(("target_x", "target_y", "z_offset", "rotation",
                                         "angle_of_attack", "distance", "roll", "field_of_view",
                                         "far_z", "near_z"), values))})
    require(reader.offset == len(data), "W3C is not a fully consumed classic layout")
    if count:
        # 同一字节若也可完整解析为 13-float 扩展布局，则没有唯一 classic 证明。
        extended = WireReader(data)
        extended_valid = False
        try:
            extended_count = extended.header(0, 53)
            for _ in range(extended_count):
                extra_values = struct.unpack("<13f", extended.take(52))
                require(all(math.isfinite(value) for value in extra_values), "non-finite extended W3C field")
                extended.text()
            extended_valid = extended.offset == len(data)
        except (RuntimeError, ValueError, struct.error):
            pass
        require(not extended_valid, "W3C layout is ambiguous; independent classic proof unavailable")
    return {"data": data, "sha256": hashlib.sha256(data).hexdigest(), "rows": rows, "count": count}


def read_wire(path, parser):
    require(path.is_file() and 8 <= path.stat().st_size <= MAX_MEMBER_BYTES,
            "auxiliary file is outside the independent byte budget")
    return parser(path.read_bytes())


def f32_bytes(value):
    require(type(value) in (int, float) and math.isfinite(value) and abs(value) <= 1000000,
            "region edit must be a finite numeric coordinate within ±1000000")
    return struct.pack("<f", value)


def f32(value):
    return struct.unpack("<f", f32_bytes(value))[0]


def choose_region(source):
    for row in source["rows"]:
        bounds = row["bounds"]
        if not (-999998 <= bounds["left"] < bounds["right"] <= 999998 and
                -1000000 <= bounds["bottom"] < bounds["top"] <= 1000000):
            continue
        # 平移 1 世界单位，使用明确的 f32 round-trip 值；只开放这两个四字节槽。
        target = {key: f32(bounds[key] + 1) for key in ("left", "right")}
        if target["left"] < target["right"] and all(target[key] != bounds[key] for key in target):
            return row, target
    raise RuntimeError("no real existing region has valid bounds for the narrow two-coordinate edit")


def expected_region_bytes(source, row, target):
    require(set(target) == {"left", "right"}, "wire oracle authorizes exactly left/right")
    require(target["left"] < target["right"], "wire oracle rejected inverted region bounds")
    expected = bytearray(source["data"])
    for key, relative in (("left", 0), ("right", 8)):
        position = row["start"] + relative
        expected[position:position + 4] = f32_bytes(target[key])
    return bytes(expected)


def verify_region_delta(source, candidate, row, target):
    expected = expected_region_bytes(source, row, target)
    require(candidate["data"] == expected,
            "candidate W3R differs outside the exact two authorized four-byte f32 slots")
    require(candidate["count"] == source["count"] and candidate["sha256"] != source["sha256"],
            "region wire proof lost a record or is a no-op")
    require(candidate["rows"][row["index"]]["row_key"] == row["row_key"],
            "region name/creation row locator changed")
    spans = [{"field": key, "offset": row["start"] + relative, "bytes": 4,
              "before_hex": source["data"][row["start"] + relative:row["start"] + relative + 4].hex(),
              "after_hex": f32_bytes(target[key]).hex(), "value": target[key]}
             for key, relative in (("left", 0), ("right", 8))]
    return {"source_member_sha256": source["sha256"], "candidate_member_sha256": candidate["sha256"],
            "record_count": source["count"], "selected_row_index": row["index"],
            "row_key": row["row_key"], "authorized_f32_spans": spans,
            "all_other_records_names_weather_sound_color_and_record_tails_exact": True,
            "whole_member_equals_source_with_only_authorized_slots_replaced": True}


class EntityHttpServer(VariableHttpServer):
    def start(self):
        super().start()
        listed = json.loads((self.evidence.output / f"{self.label}.tools-list.json").read_text(encoding="utf-8"))
        definitions = {tool.get("name"): tool for tool in listed.get("tools", [])}
        require(ENTITY_TOOLS <= definitions.keys(), "actual C1 entity tools are not registered")
        query = definitions["entities_query"].get("inputSchema", {})
        write = definitions["entities_set_fields"].get("inputSchema", {})
        require(query.get("type") == "object" and set(query.get("required", [])) == {"kind"},
                "entities_query contract differs from the frozen C1 schema")
        require(set(query.get("properties", {}).get("kind", {}).get("enum", [])) == set(ENTITY_MEMBERS),
                "entities_query does not expose the exact four C1 categories")
        require(write.get("type") == "object" and
                {"project_identity", "expected_revision", "kind", "expected_member_sha256", "changes"}
                == set(write.get("required", [])), "entities_set_fields is missing a frozen publication guard")
        require(write.get("properties", {}).get("expected_revision", {}).get("type") == "string",
                "C1 expected_revision must be an opaque string")
        item = write.get("properties", {}).get("changes", {}).get("items", {})
        require(set(item.get("required", [])) == {"source_index", "row_key", "field", "expected_raw", "value"}
                and item.get("properties", {}).get("value", {}).get("type") == "string",
                "C1 changes schema differs from the actual field locator/raw-before contract")
        self.evidence.check(f"{self.label}-actual-c1-tool-schemas", {
            "names": sorted(ENTITY_TOOLS), "schema_sha256": json_sha({name: definitions[name] for name in sorted(ENTITY_TOOLS)}),
            "existing_A_and_B_tools_retained": True})
        return self


def unsigned(value, label, nonzero=False):
    require(isinstance(value, str) and re.fullmatch(r"0|[1-9][0-9]*", value),
            f"{label} is not a canonical unsigned decimal string")
    number = int(value)
    require(number <= 2**64 - 1 and (not nonzero or number > 0), f"{label} is out of range")
    return number


def fields(row):
    require(isinstance(row.get("fields"), list), "actual entity row has no field array")
    result = {}
    for field in row["fields"]:
        require(isinstance(field, dict) and isinstance(field.get("id"), str) and field["id"] not in result,
                "actual entity row has an invalid/duplicate field")
        require(isinstance(field.get("raw"), str) and type(field.get("writable")) is bool,
                "entity field is missing exact raw-before/writeability")
        result[field["id"]] = field
    return result


def query_entities(server, kind):
    rows, initial, start = [], None, 0
    for _ in range(MAX_PAGES):
        parameters = {"kind": kind, "page": {"start_index": str(start), "limit": 128}}
        if initial is not None:
            parameters.update(context(initial))
            parameters["expected_member_sha256"] = initial["member"]["snapshot_token"]
        reply = server.tool("entities_query", parameters)
        context(reply)
        require(isinstance(reply.get("binding"), dict) and isinstance(reply.get("member"), dict),
                "actual entity query is not bound to a document/member snapshot")
        member = reply["member"]
        require(member.get("kind") == kind and normalized_member(member.get("member_name", "")) == ENTITY_MEMBERS[kind],
                "entity query returned another member/category")
        require(isinstance(reply.get("rows"), list), "actual entity query has no row page")
        if initial is None:
            initial = reply
        else:
            require(context(reply) == context(initial) and reply["binding"] == initial["binding"] and
                    member == initial["member"], "entity paging crossed a revision/member snapshot")
        require(member.get("state") in ("ready", "missing"), "entity query hid an unsafe source state")
        if member["state"] == "missing":
            require(member.get("member_sha256") is None and member.get("snapshot_token") == "missing" and
                    not reply["rows"] and reply.get("next_cursor") is None, "missing member was fabricated as a real table")
            return rows, reply
        require(isinstance(member.get("member_sha256"), str) and HEX_SHA.fullmatch(member["member_sha256"]) and
                member.get("snapshot_token") == member["member_sha256"], "entity member has no exact source/current SHA")
        for index, row in enumerate(reply["rows"], start):
            require(isinstance(row, dict) and unsigned(row.get("source_index"), "source_index") == index and
                    row.get("locator_scope") == "snapshot-revision" and isinstance(row.get("row_key"), str) and row["row_key"],
                    "entity query locator is missing or not a contiguous snapshot row")
            fields(row)
            rows.append(row)
        cursor = reply.get("next_cursor")
        if cursor is None:
            require(unsigned(member.get("record_count"), "record_count") == len(rows), "entity count disagrees with complete pagination")
            return rows, reply
        require(isinstance(cursor, dict) and cursor.get("kind") == kind and cursor.get("binding") == reply["binding"],
                "entity next cursor is not bound to the actual current page")
        following = unsigned(cursor.get("next_index"), "next_index")
        require(following == start + len(reply["rows"]) and following > start, "entity paging did not make exact forward progress")
        start = following
    raise RuntimeError("entity query exceeded the independent page budget")


def raw_f32(value):
    return "f32:" + format(struct.unpack("<I", struct.pack("<f", value))[0], "08x")


def check_region_query(rows, wire):
    require(len(rows) == wire["count"], "HTTP region count differs from independent native W3R")
    for actual, native in zip(rows, wire["rows"]):
        require(actual["row_key"] == native["row_key"] and actual.get("creation_number") == native["creation_number"] and
                actual.get("name", {}).get("raw_hex") == native["name_hex"], "HTTP region locator/name differs from native bytes")
        actual_fields = fields(actual)
        for key, value in native["bounds"].items():
            field = actual_fields.get(key, {})
            require(field.get("type") == "float32" and field.get("raw") == raw_f32(value) and
                    field.get("value") == value and field.get("writable") is True,
                    "HTTP region bound differs from native f32 raw bits/value or writable policy")
        require(actual_fields.get("name", {}).get("writable") is False, "C1 incorrectly exposes region name editing")


def check_camera_query(rows, wire):
    require(len(rows) == wire["count"], "HTTP camera count differs from independent classic W3C")
    for actual, native in zip(rows, wire["rows"]):
        require(actual["row_key"] == native["row_key"] and actual.get("name", {}).get("raw_hex") == native["name_hex"],
                "HTTP camera row locator/name differs from native bytes")
        actual_fields = fields(actual)
        for key, value in native["values"].items():
            require(actual_fields.get(key, {}).get("raw") == raw_f32(value), "HTTP camera raw f32 differs from native bytes")
        require({key for key, field in actual_fields.items() if field["writable"]} ==
                {"target_x", "target_y", "z_offset", "distance"}, "camera writable scope exceeds/fails the C1 field contract")


def change(row, field, value):
    return {"source_index": row["source_index"], "row_key": row["row_key"], "field": field,
            "expected_raw": fields(row)[field]["raw"], "value": str(value)}


def write_request(reply, changes):
    return {**context(reply), "kind": "regions", "expected_member_sha256": reply["member"]["member_sha256"],
            "changes": changes}


def unchanged_after_refusal(server, evidence, label, request, expected_code, current_rows, current_reply):
    rejected = server.rejection("entities_set_fields", request, expected_code)
    rows, reply = query_entities(server, "regions")
    require(rows == current_rows and reply == current_reply, "refused entity batch changed rows, member SHA, identity or revision")
    evidence.check(label, {"refusal": rejected, "all_current_rows_and_binding_exact": True,
                           "unsaved_edited_overlay_preserved": True})


def assert_write_receipt(edited, before_reply, before_row, after_reply, after_row):
    require(edited.get("changed") is True and edited.get("saved") is False and edited.get("runtime_verified") is False,
            "entity field write claimed a save/runtime proof or did not publish a real change")
    require(edited.get("kind") == "regions" and unsigned(edited.get("applied_fields"), "applied_fields") == 2 and
            unsigned(edited.get("changed_rows"), "changed_rows") == 1, "entity receipt does not describe the exact 2-field/1-row command")
    require(edited.get("before") == {"member": before_reply["member"], "rows": [before_row]} and
            edited.get("after") == {"member": after_reply["member"], "rows": [after_row]},
            "entity receipt before/after is not the complete actual touched row/member snapshot")
    require(edited.get("before_binding") == before_reply["binding"],
            "entity complete before receipt has another snapshot/revision binding")
    session = unsigned(edited.get("session_identity"), "session_identity", True)
    command = unsigned(edited.get("command_identity"), "command_identity", True)
    old_revision = unsigned(edited.get("session_revision_before"), "session_revision_before")
    new_revision = unsigned(edited.get("session_revision_after"), "session_revision_after")
    require(new_revision == old_revision + 1 and context(edited) == context(after_reply),
            "entity command did not advance exactly one actual session revision")
    require(isinstance(edited.get("changes"), dict), "entity command has no original host changes receipt")
    return {"session_identity": str(session), "command_identity": str(command),
            "session_revision_before": str(old_revision), "session_revision_after": str(new_revision)}


def inputs():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("server", "cli", "source", "workspace", "ydwe-root", "war3-root", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--qt-bin", type=Path, help="仅向自有子进程 PATH 添加 Qt 运行库目录")
    parser.add_argument("--expected-source-sha256", required=True, help="授权源归档的64位小写SHA-256；不是目录aggregate")
    parser.add_argument("--start-timeout", type=float, default=60)
    parser.add_argument("--request-timeout", type=float, default=60)
    parser.add_argument("--save-timeout", type=float, default=900)
    parser.add_argument("--cli-timeout", type=float, default=60)
    args = parser.parse_args()
    require(HEX_SHA.fullmatch(args.expected_source_sha256), "expected source archive SHA must be 64 lowercase hex digits")
    for key in ("start_timeout", "request_timeout", "save_timeout", "cli_timeout"):
        require(0 < getattr(args, key) <= 3600, "timeouts must be positive and no greater than one hour")
    args.output = args.output.resolve()
    require(not args.output.is_relative_to(args.workspace.resolve()), "evidence output must be outside the input workspace")
    args.output.mkdir(parents=True, exist_ok=False)
    return args


def prepare(args, evidence):
    for name in ("server", "cli", "source", "workspace", "ydwe_root", "war3_root", "qt_bin"):
        value = getattr(args, name)
        if value is not None:
            setattr(args, name, value.resolve(strict=True))
    require(args.server.is_file() and args.cli.is_file() and args.source.is_file(), "binary/source path is not a file")
    require(args.source.suffix.lower() in (".w3x", ".w3m"), "source must be a map archive")
    require(args.workspace.is_dir() and (args.ydwe_root / "ui").is_dir() and args.war3_root.is_dir(),
            "workspace or trusted root/ui directory is missing")
    source_before = sha(args.source)
    require(source_before == args.expected_source_sha256, "authorized source archive SHA mismatch")
    workspace_before = inventory(args.workspace)
    evidence.artifact("source-workspace.before.json", workspace_before)
    evidence.receipt.update({"source": str(args.source), "workspace": str(args.workspace),
        "source_sha256_before": source_before, "expected_source_archive_sha256": args.expected_source_sha256,
        "server": str(args.server), "server_sha256": sha(args.server), "cli": str(args.cli), "cli_sha256": sha(args.cli),
        "ydwe_root": str(args.ydwe_root), "ui_data": str(args.ydwe_root / "ui"), "war3_root": str(args.war3_root),
        "workspace_inventory_sha256_before": json_sha(workspace_before)})
    env = dict(os.environ)
    env.pop("FORGE_MCP_ACCESS_TOKEN", None)
    path_entries = [str(args.server.parent), str(args.cli.parent)]
    if args.qt_bin:
        require(args.qt_bin.is_dir(), "Qt runtime directory does not exist")
        path_entries.insert(0, str(args.qt_bin))
    env["PATH"] = os.pathsep.join(path_entries + [env.get("PATH", "")])
    env["FORGE_MCP_YDWE_ROOT"], env["FORGE_MCP_WAR3_ROOT"] = str(args.ydwe_root), str(args.war3_root)
    evidence.checkpoint("input-identities-captured")
    return env, source_before, workspace_before


def finish_candidate(args, env, evidence, servers, source_server, source_before, workspace_before,
                     source_members, source_audit, source_regions, source_cameras, original,
                     selected, target, expected_wire, command, after_rows):
    require(source_server.envelope is not None, "C1 edit lost the original active transaction envelope")
    preview = source_server.tool("preview_edit", source_server.envelope)
    evidence.artifact("transaction.preview.json", preview)
    patches = preview.get("changes")
    require(isinstance(patches, list) and len(patches) == 1, "C1 preview contains anything beyond its one W3R native patch")
    patch = patches[0]
    require(isinstance(patch, dict) and patch.get("kind") == "map-data" and
            normalized_member(patch.get("path", "")) == "war3map.w3r" and
            patch.get("before_sha256") == source_regions["sha256"] and
            patch.get("after_sha256") == expected_wire["sha256"] and
            patch["before_sha256"] != patch["after_sha256"],
            "C1 preview is not bound to the independent original/final W3R byte plan")
    validated = source_server.tool("validate_edit", source_server.envelope)
    require(validated.get("satisfied") is True and validated.get("highest_proven_level", -1) >= 0,
            "latest entity transaction generation did not validate")
    evidence.check("original-transaction-preview-and-validation-bound-to-cumulative-final-native-patch", {
        "preview": preview, "validation": validated, "envelope": source_server.envelope,
        "final_command": command, "final_member_sha256": expected_wire["sha256"]})
    candidate = args.output / ("mcp-entities-c1-candidate" + args.source.suffix.lower())
    require(not candidate.exists(), "candidate already exists")
    saved = source_server.tool("save_map_edit_candidate", {"source_map_path": str(args.source),
        "output_path": str(candidate), "expected_source_sha256": source_before,
        **source_server.envelope, "script_target": "source"}, timeout=args.save_timeout)
    require(candidate.is_file() and saved.get("candidate_kind") == "mpq-map-archive" and
            saved.get("source_sha256") == source_before and saved.get("candidate_sha256") == sha(candidate) and
            saved.get("script_rebuilt") is True and saved.get("native_editor_verified") is False and
            saved.get("game_runtime_verified") is False, "save receipt archive identities or evidence claims are invalid")
    require(isinstance(saved.get("output_path"), str) and Path(saved["output_path"]).resolve(strict=True) == candidate,
            "save receipt does not identify the actual output candidate")
    require(type(saved.get("applied_field_edit_count")) is int and saved["applied_field_edit_count"] == 2,
            "save did not consume exactly two cumulative source-to-final field edits")
    trigger_receipt = saved.get("trigger_transaction_receipt")
    if trigger_receipt is not None:
        require(isinstance(trigger_receipt, dict) and trigger_receipt.get("transaction_id") == source_server.envelope["transaction_id"] and
                trigger_receipt.get("transaction_active") is True and trigger_receipt.get("workspace_rebound") is False,
                "save receipt does not preserve the original active transaction identity")
    region_receipts = [row for row in saved.get("edited_members", [])
                       if normalized_member(row.get("name", "")) == "war3map.w3r"]
    require(len(region_receipts) == 1 and region_receipts[0].get("original_sha256") == source_regions["sha256"] and
            region_receipts[0].get("saved_sha256") == expected_wire["sha256"],
            "exact native region save receipt does not bind source and independently requested final bytes")
    evidence.receipt["saved"], evidence.receipt["candidate"] = saved, str(candidate)
    evidence.check("original-save-service-produced-real-c1-candidate-with-two-cumulative-fields", saved)

    source_server.rollback()
    for kind in ENTITY_MEMBERS:
        rows, reply = query_entities(source_server, kind)
        require(rows == original[kind]["rows"] and reply["member"] == original[kind]["reply"]["member"],
                f"original transaction rollback did not restore the exact {kind} source table/member")
    evidence.check("source-entity-overlay-rolled-back-after-save", {"all_four_original_tables_exact": True})
    source_server.close()

    candidate_members = list_members(args, env, evidence, candidate, "candidate")
    candidate_workspace = args.output / "candidate-workspace"
    candidate_manifest = extract_members(args, env, evidence, candidate, list(candidate_members.values()), candidate_workspace, "candidate")
    require("war3map.w3r" in candidate_manifest and
            candidate_manifest["war3map.w3r"]["sha256"] == expected_wire["sha256"] and
            region_receipts[0]["saved_sha256"] == candidate_manifest["war3map.w3r"]["sha256"],
            "save receipt/final overlay SHA differs from actual independently extracted candidate W3R")
    protected = {}
    for member in PROTECTED_MEMBERS:
        before, after = source_audit.get(member), candidate_manifest.get(member)
        require((before is None) == (after is None) and (before is None or before["sha256"] == after["sha256"]),
                f"unchanged protected member changed: {member}")
        protected[member] = {"present": before is not None, "source_sha256": before["sha256"] if before else None,
                             "candidate_sha256": after["sha256"] if after else None, "body_inspected": False}
    evidence.check("original-trigger-pair-all-seven-native-object-members-tiara-and-other-map-members-preserved", protected)
    actual_wire = read_wire(candidate_workspace / safe_member(candidate_members["war3map.w3r"]), read_regions_bytes)
    proof = verify_region_delta(source_regions, actual_wire, selected, target)
    evidence.artifact("candidate-region.actual-native-byte-proof.json", proof)
    evidence.check("actual-candidate-is-original-w3r-with-exactly-two-authorized-f32-slots-replaced", proof)
    actual_cameras = read_wire(candidate_workspace / safe_member(candidate_members["war3map.w3c"]), read_cameras_bytes)
    require(actual_cameras["data"] == source_cameras["data"], "candidate classic W3C changed")

    reopened = EntityHttpServer(args, env, candidate_workspace, "candidate", evidence)
    servers.append(reopened)
    reopened.start()
    reopened_regions, reopened_reply = query_entities(reopened, "regions")
    check_region_query(reopened_regions, actual_wire)
    require(reopened_regions == after_rows and
            reopened_reply["member"]["member_sha256"] == candidate_manifest["war3map.w3r"]["sha256"],
            "second independent HTTP service did not read the exact saved final region rows/raw coordinates")
    for kind in ("units", "doodads", "cameras"):
        rows, reply = query_entities(reopened, kind)
        require(rows == original[kind]["rows"] and reply["member"] == original[kind]["reply"]["member"],
                f"second HTTP service changed or lost an original {kind} field/record")
        if kind == "cameras":
            check_camera_query(rows, actual_cameras)
    require(reopened.record["initial_revision"]["revision"]["ui_catalog_sha256"] ==
            source_server.record["initial_revision"]["revision"]["ui_catalog_sha256"], "candidate reopened with another actual catalog")
    evidence.artifact("candidate-region.http-readback.json", {"rows": reopened_regions, "reply": reopened_reply})
    evidence.check("second-owned-http-service-exact-saved-region-and-other-entities-readback", {
        "selected_source_index": str(selected["index"]), "row_key": reopened_regions[selected["index"]]["row_key"],
        "final_coordinates": target, "candidate_archive_sha256": sha(candidate),
        "candidate_directory_aggregate_sha256": reopened_reply["binding"]["source_sha256"],
        "exact_saved_native_member_sha256": candidate_manifest["war3map.w3r"]["sha256"],
        "row_locator_requeried_in_new_revision": True, "permanent_ID_claimed": False})
    reopened.close()
    require(sha(args.source) == source_before and inventory(args.workspace) == workspace_before,
            "original source archive or input workspace changed before cleanup")


def main():
    args = inputs()
    evidence = Evidence(args.output)
    evidence.receipt.update({"schema": "audastra-mcp-entities-c1-http-probe-v1",
        "scope": "existing-region-left-right-fields-only",
        "whole_C_group_verified": False, "row_locators_across_revisions_claimed_permanent": False})
    servers, source_before, workspace_before = [], None, None
    try:
        env, source_before, workspace_before = prepare(args, evidence)
        source_members = list_members(args, env, evidence, args.source, "source")
        required = {"war3map.wtg", "war3map.wct", "war3map.w3r", "war3map.w3c"}
        require(required <= source_members.keys(), "actual archive lacks the classic region/camera and trigger sources needed by this probe")
        audit_names = [source_members[name] for name in ("war3map.w3r",) + PROTECTED_MEMBERS if name in source_members]
        audit_dir = args.output / "source-member-hashes"
        source_audit = extract_members(args, env, evidence, args.source, audit_names, audit_dir, "source-audit")
        for name, original in source_audit.items():
            workspace_member = args.workspace / safe_member(source_members[name])
            require(workspace_member.is_file() and sha(workspace_member) == original["sha256"],
                    f"input workspace {name} differs from the authorized archive")
        source_regions = read_wire(audit_dir / safe_member(source_members["war3map.w3r"]), read_regions_bytes)
        source_cameras = read_wire(audit_dir / safe_member(source_members["war3map.w3c"]), read_cameras_bytes)
        selected, target = choose_region(source_regions)
        expected_bytes = expected_region_bytes(source_regions, selected, target)
        expected_wire = read_regions_bytes(expected_bytes)
        wire_proof = verify_region_delta(source_regions, expected_wire, selected, target)
        evidence.artifact("requested-region.independent-wire-plan.json", wire_proof)

        source_server = EntityHttpServer(args, env, args.workspace, "source", evidence)
        servers.append(source_server)
        source_server.start()
        original = {}
        for kind in ENTITY_MEMBERS:
            rows, reply = query_entities(source_server, kind)
            original[kind] = {"rows": rows, "reply": reply}
            name = ENTITY_MEMBERS[kind]
            member = reply["member"]
            require((name in source_members) == (member["state"] == "ready"), "HTTP entity presence differs from the actual archive")
            require(name not in source_members or member["member_sha256"] == source_audit[name]["sha256"],
                    "HTTP entity member is not the independently extracted source")
            evidence.artifact(f"source-{kind}.before.json", original[kind])
        check_region_query(original["regions"]["rows"], source_regions)
        check_camera_query(original["cameras"]["rows"], source_cameras)
        require(all(not field["writable"] for row in original["doodads"]["rows"] for field in fields(row).values()),
                "C1 exposed writable doodad fields")
        directory_sha = source_server.record["initial_revision"]["revision"]["source_sha256"]
        require(original["regions"]["reply"]["binding"].get("source_sha256") == directory_sha,
                "entity binding does not identify the actual expanded-source aggregate")
        evidence.check("actual-four-category-source-query-and-classic-wire-binding", {
            "source_archive_sha256": source_before, "expanded_directory_aggregate_sha256": directory_sha,
            "hashes_bind_distinct_inputs": True, "region_count": source_regions["count"],
            "camera_count": source_cameras["count"], "doodad_fields_read_only": True})

        started = source_server.tool("begin_edit", {"expected_revision": source_server.record["initial_revision"]["revision"]})
        require(isinstance(started.get("transaction_id"), str) and isinstance(started.get("base_revision"), dict),
                "original begin_edit did not identify the actual transaction/base revision")
        source_server.envelope = {"transaction_id": started["transaction_id"], "expected_revision": started["base_revision"]}
        transaction_envelope = copy.deepcopy(source_server.envelope)
        before_rows, before_reply = query_entities(source_server, "regions")
        require(before_rows == original["regions"]["rows"], "begin_edit changed the actual original region table")
        before_row = before_rows[selected["index"]]
        edits = [change(before_row, key, target[key]) for key in ("left", "right")]
        parameters = write_request(before_reply, edits)
        edited = source_server.tool("entities_set_fields", parameters)
        after_rows, after_reply = query_entities(source_server, "regions")
        check_region_query(after_rows, expected_wire)
        require(after_reply["member"]["member_sha256"] == expected_wire["sha256"],
                "active overlay member SHA differs from independent exact requested W3R bytes")
        expected_rows = copy.deepcopy(before_rows)
        for field in expected_rows[selected["index"]]["fields"]:
            if field["id"] in target:
                field["raw"], field["value"] = raw_f32(target[field["id"]]), target[field["id"]]
        require(after_rows == expected_rows, "active HTTP edit changed any other field, row, name or locator")
        command = assert_write_receipt(edited, before_reply, before_row, after_reply, after_rows[selected["index"]])
        require(source_server.envelope == transaction_envelope, "first entity command changed the original transaction identity/base revision")
        evidence.artifact("active-region.after.json", {"rows": after_rows, "reply": after_reply, "write": edited})
        evidence.check("actual-two-f32-fields-published-once-with-complete-receipt", {"command": command,
            "requested_changes": edits, "exact_requested_member_sha256": expected_wire["sha256"]})

        first_command, first_wire_sha = command, expected_wire["sha256"]
        first_rows, first_reply = after_rows, after_reply
        target = {key: f32(selected["bounds"][key] + 2) for key in ("left", "right")}
        require(target["left"] < target["right"] and
                all(target[key] != fields(first_rows[selected["index"]])[key]["value"] for key in target),
                "second region edit has invalid final bounds")
        expected_wire = read_regions_bytes(expected_region_bytes(source_regions, selected, target))
        require(expected_wire["sha256"] != first_wire_sha, "second successful edit would be a no-op")
        second_edits = [change(first_rows[selected["index"]], key, target[key]) for key in ("left", "right")]
        second = source_server.tool("entities_set_fields", write_request(first_reply, second_edits))
        after_rows, after_reply = query_entities(source_server, "regions")
        check_region_query(after_rows, expected_wire)
        require(after_reply["member"]["member_sha256"] == expected_wire["sha256"],
                "second active overlay SHA differs from source with the exact final two f32 replacements")
        expected_rows = copy.deepcopy(before_rows)
        for field in expected_rows[selected["index"]]["fields"]:
            if field["id"] in target:
                field["raw"], field["value"] = raw_f32(target[field["id"]]), target[field["id"]]
        require(after_rows == expected_rows, "second entity command changed an unauthorized field or row")
        command = assert_write_receipt(second, first_reply, first_rows[selected["index"]], after_reply, after_rows[selected["index"]])
        # standalone 每次提交使用原 Core 的隔离会话；会话/命令组合是本次真实回执，
        # 跨调用连续性由原活动事务身份、opaque revision 和完整 before/after bytes 证明。
        require((command["session_identity"], command["command_identity"]) !=
                (first_command["session_identity"], first_command["command_identity"]) and
                context(first_reply)["project_identity"] == context(after_reply)["project_identity"] and
                context(first_reply)["expected_revision"] != context(after_reply)["expected_revision"] and
                source_server.envelope == transaction_envelope,
                "second command reused an actual receipt tuple or did not advance the public CAS revision")
        evidence.artifact("active-region.final.json", {"rows": after_rows, "reply": after_reply, "write": second})
        evidence.check("second-successful-command-updates-same-fields-for-cumulative-final-freeze", {
            "first_command": first_command, "second_command": command, "requested_changes": second_edits,
            "intermediate_member_sha256": first_wire_sha, "final_member_sha256": expected_wire["sha256"],
            "expected_frozen_source_to_final_field_count": 2,
            "standalone_core_session_identity_claimed_continuous": False})

        current_row = after_rows[selected["index"]]
        restore = change(current_row, "left", selected["bounds"]["left"])
        late_invalid = change(current_row, "right", selected["bounds"]["left"] - 1)
        unchanged_after_refusal(source_server, evidence, "valid-restore-prefix-invalid-late-bounds-atomic-refusal",
            write_request(after_reply, [restore, late_invalid]), "MAP_FIELD_ROW_INVALID", after_rows, after_reply)
        unchanged_after_refusal(source_server, evidence, "stale-opaque-revision-refused-current-overlay-preserved",
            parameters, "ENTITY_REVISION_CONFLICT", after_rows, after_reply)
        wrong_member = write_request(after_reply, [restore]); wrong_member["expected_member_sha256"] = "0" * 64
        unchanged_after_refusal(source_server, evidence, "wrong-member-sha-refused",
            wrong_member, "ENTITY_MEMBER_STALE", after_rows, after_reply)
        wrong_row = copy.deepcopy(restore); wrong_row["row_key"] += "00"
        unchanged_after_refusal(source_server, evidence, "wrong-row-locator-refused",
            write_request(after_reply, [wrong_row]), "MAP_FIELD_ROW_STALE", after_rows, after_reply)
        readonly = change(current_row, "name", fields(current_row)["name"]["raw"])
        unchanged_after_refusal(source_server, evidence, "valid-prefix-readonly-region-name-atomic-refusal",
            write_request(after_reply, [restore, readonly]), "MAP_FIELD_READ_ONLY", after_rows, after_reply)
        wrong_raw = copy.deepcopy(restore); wrong_raw["expected_raw"] = raw_f32(selected["bounds"]["left"])
        unchanged_after_refusal(source_server, evidence, "stale-exact-f32-before-value-refused",
            write_request(after_reply, [wrong_raw]), "MAP_FIELD_VALUE_STALE", after_rows, after_reply)
        for kind in ("units", "doodads", "cameras"):
            rows, _ = query_entities(source_server, kind)
            require(rows == original[kind]["rows"], f"region edit/refusals changed the active {kind} table")
        evidence.check("all-other-active-entity-categories-unchanged", {"kinds": ["units", "doodads", "cameras"]})

        finish_candidate(args, env, evidence, servers, source_server, source_before, workspace_before,
                         source_members, source_audit, source_regions, source_cameras, original,
                         selected, target, expected_wire, command, after_rows)
        require(sha(args.source) == source_before, "source archive changed during HTTP probe")
        require(inventory(args.workspace) == workspace_before, "input expanded workspace changed during HTTP probe")
        require(not evidence.receipt["cleanup_errors"], "owned server cleanup failed")
        evidence.receipt["passed"] = True
        evidence.checkpoint("passed-independent-c1-http-and-archive-readback")
    except Exception as error:
        evidence.receipt["failed_stage"] = evidence.receipt["stage"]
        evidence.receipt["error"] = evidence.scrub(str(error))
        evidence.checkpoint("failed")
    finally:
        for server in reversed(servers):
            try:
                server.close()
            except Exception as error:
                evidence.receipt["cleanup_errors"].append({"server": server.label, "close_error": str(error)})
        try:
            if source_before is not None:
                source_after = sha(args.source)
                evidence.receipt.update({"source_sha256_after": source_after, "source_unchanged": source_after == source_before})
            if workspace_before is not None:
                after_inventory = inventory(args.workspace)
                evidence.artifact("source-workspace.after.json", after_inventory)
                evidence.receipt.update({"workspace_inventory_sha256_after": json_sha(after_inventory),
                                         "source_workspace_unchanged": after_inventory == workspace_before})
        except Exception as error:
            evidence.receipt["cleanup_errors"].append({"identity_recheck_error": str(error)})
        if (evidence.receipt["cleanup_errors"] or evidence.receipt.get("source_unchanged") is False or
                evidence.receipt.get("source_workspace_unchanged") is False):
            evidence.receipt["passed"] = False
        evidence.checkpoint()
        evidence.log.close()
    print(json.dumps({"passed": evidence.receipt["passed"], "receipt": str(args.output / "receipt.json"),
                      "candidate": evidence.receipt.get("candidate"), "error": evidence.receipt.get("error")}, ensure_ascii=False))
    return 0 if evidence.receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
