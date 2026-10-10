"""C2 独立 HTTP 单位复制保存、重复绝对朝向与 classic W3do 全字节回读。

传输、原事务、CLI、凭据脱敏、来源清单及自有进程清理复用 A/B/C1。
独立 oracle 只依据经典 reader/writer 字段布局；不调用新 C++ copy/rotate 逻辑。
仅写新证据目录，不回写源归档或输入目录，不宣称整个 C 组、GUI 或游戏验收。
"""

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import struct

from mcp_object_http_probe import (
    Evidence, OBJECT_MEMBERS, extract_members, inventory, json_sha, list_members,
    normalized_member, require, safe_member, sha,
)
from mcp_variables_http_probe import context
from mcp_entities_http_probe import (
    ENTITY_MEMBERS, HEX_SHA, EntityHttpServer, change, check_region_query, fields, prepare,
    query_entities, raw_f32, read_regions_bytes, read_wire, unsigned,
)


UNIT_MEMBER = ENTITY_MEMBERS["units"]
PLACEMENT_TOOLS = {"entities_copy", "entities_rotate"}
PROTECTED_MEMBERS = ("war3map.wtg", "war3map.wct", "tiara.ai", "tiara.blp",
    "war3map.doo", "war3map.w3r", "war3map.w3c", "war3map.w3i", "war3map.w3e",
    "war3map.wpm", "war3map.w3s", "war3map.wts", "war3map.imp") + OBJECT_MEMBERS
MAX_WIRE_BYTES, MAX_UNITS, MAX_NESTED = 64 * 1024 * 1024, 100000, 1000000


def f32(value):
    require(type(value) in (int, float) and math.isfinite(value), "nonfinite independent f32 input")
    try:
        result = struct.unpack("<f", struct.pack("<f", value))[0]
    except (OverflowError, struct.error) as error:
        raise RuntimeError("independent f32 input overflows") from error
    require(math.isfinite(result), "independent f32 result is nonfinite")
    return result


class UnitReader:
    def __init__(self, data):
        require(isinstance(data, bytes) and 16 <= len(data) <= MAX_WIRE_BYTES,
                "unit native member exceeds the independent wire budget")
        self.data, self.offset, self.nested = data, 0, 0

    def take(self, size):
        require(0 <= size <= len(self.data) - self.offset, "truncated unit native field")
        start = self.offset
        self.offset += size
        return self.data[start:self.offset]

    def integer(self):
        return struct.unpack("<i", self.take(4))[0]

    def floating(self):
        value = struct.unpack("<f", self.take(4))[0]
        require(math.isfinite(value), "nonfinite unit native float")
        return value

    def count(self, width, nested=True):
        value = self.integer()
        require(0 <= value <= (len(self.data) - self.offset) // width, "invalid/truncated unit count")
        if nested:
            require(value <= MAX_NESTED - self.nested, "unit nested record budget exceeded")
            self.nested += value
        else:
            require(value <= MAX_UNITS, "unit record budget exceeded")
        return value

    def choices(self):
        return [(self.take(4), self.integer()) for _ in range(self.count(8))]


def parse_units_layout(data, skin=False):
    reader = UnitReader(data)
    require(reader.take(4) == b"W3do" and reader.integer() == 8 and reader.integer() == 11,
            "only classic W3do v8/subversion11 is accepted")
    count = reader.count(111 if skin else 107, False)
    rows = []
    for index in range(count):
        start = reader.offset
        row = {"index": index, "start": start, "rawcode": reader.take(4), "variation": reader.integer()}
        row["position"] = [reader.floating() for _ in range(3)]
        row["facing"] = reader.floating()
        row["scale"] = [reader.floating() for _ in range(3)]
        if skin:
            row["skin"] = reader.take(4)
        row["flags"] = reader.take(1)[0]
        require(row["flags"] < 0x20, "unit flags indicate unsupported native layout")
        row["owner"] = reader.integer()
        row["unknown_owner_bytes"] = reader.take(2)
        for key in ("hit_points", "mana", "dropped_item_table"):
            row[key] = reader.integer()
        row["item_sets"] = [reader.choices() for _ in range(reader.count(4))]
        row["gold"], row["target_acquisition"] = reader.integer(), reader.floating()
        for key in ("hero_level", "hero_strength", "hero_agility", "hero_intelligence"):
            row[key] = reader.integer()
        row["inventory"] = [(reader.integer(), reader.take(4)) for _ in range(reader.count(8))]
        row["abilities"] = []
        for _ in range(reader.count(12)):
            rawcode, autocast, level = reader.take(4), reader.integer(), reader.integer()
            require(autocast in (0, 1), "unit autocast is not a native boolean")
            row["abilities"].append((rawcode, autocast, level))
        mode = reader.integer()
        require(mode in (-1, 0, 1, 2), "unknown unit random branch")
        row["random_mode"] = mode
        row["random"] = (reader.take(4) if mode == 0 else
                         (reader.integer(), reader.integer()) if mode == 1 else
                         reader.choices() if mode == 2 else None)
        row["team_color"], row["waygate_region_id"] = reader.integer(), reader.integer()
        row["creation_offset"] = reader.offset
        row["creation_id"] = reader.integer()
        row["end"], row["bytes"] = reader.offset, data[start:reader.offset]
        row["row_key"] = "unit:" + row["rawcode"].hex() + ":" + str(row["creation_id"])
        rows.append(row)
    require(reader.offset == len(data), "unknown unit native tail")
    return {"data": data, "rows": rows, "count": count, "sha256": hashlib.sha256(data).hexdigest(),
            "nested_records": reader.nested, "layout": "skin" if skin else "classic"}


def read_units_bytes(data):
    classic = parse_units_layout(data)
    require(classic["count"] > 0, "empty unit table has no editable layout proof")
    try:
        parse_units_layout(data, True)
    except RuntimeError:
        return classic
    raise RuntimeError("classic and SkinId unit layouts are both valid; independent oracle refuses ambiguity")


def read_units(path):
    require(path.is_file() and 16 <= path.stat().st_size <= MAX_WIRE_BYTES, "invalid unit native input file")
    return read_units_bytes(path.read_bytes())


def expected_copy_bytes(source, selected, offset):
    require(selected["rawcode"] != b"sloc", "starting locations cannot be copied")
    creation = max(-1, *(row["creation_id"] for row in source["rows"])) + 1
    require(creation <= 2**31 - 1 and source["count"] < MAX_UNITS, "unit copy ID/count exhausted")
    delta = [f32(value) for value in offset]
    require(all(abs(value) <= 1000000 for value in delta), "copy offset exceeds world-coordinate bound")
    position = [f32(value + delta[axis]) for axis, value in enumerate(selected["position"])]
    require(all(abs(value) <= 1000000 for value in position) and
            all(abs(value) <= 1000 for value in selected["scale"]), "source/copy transform is unsafe")
    appended = bytearray(selected["bytes"])
    for axis, value in enumerate(position):
        struct.pack_into("<f", appended, 8 + axis * 4, value)
    require(selected["creation_offset"] == selected["end"] - 4, "creation ID is not the exact record tail")
    struct.pack_into("<i", appended, len(appended) - 4, creation)
    expected = bytearray(source["data"])
    struct.pack_into("<i", expected, 12, source["count"] + 1)
    expected.extend(appended)
    require(len(expected) <= MAX_WIRE_BYTES, "expanded copy exceeds independent wire budget")
    return bytes(expected)


def expected_rotation_bytes(source, selected, facing):
    value = f32(facing)
    require(0 <= value < f32(2 * math.pi), "rotation must be a finite absolute radian input")
    expected = bytearray(source["data"])
    struct.pack_into("<f", expected, selected["start"] + 20, value)
    return bytes(expected)


def copy_proof(source, candidate, selected, offset):
    expected = expected_copy_bytes(source, selected, offset)
    require(candidate["data"] == expected, "complete candidate native member differs from the independent exact copy oracle")
    require(candidate["count"] == source["count"] + 1 and
            all(after["bytes"] == before["bytes"] for before, after in zip(source["rows"], candidate["rows"])),
            "copy changed an original record or did not append exactly one unit")
    added = candidate["rows"][-1]
    return {"source_member_sha256": source["sha256"], "candidate_member_sha256": candidate["sha256"],
            "source_index": str(selected["index"]), "source_record_sha256": hashlib.sha256(selected["bytes"]).hexdigest(),
            "original_record_count": source["count"], "new_record_count": candidate["count"],
            "new_source_index": str(added["index"]), "new_creation_id": added["creation_id"],
            "new_rawcode_hex": added["rawcode"].hex(), "offset": dict(zip(("x", "y", "z"), offset)),
            "new_position": added["position"], "all_original_record_slices_exact": True,
            "appended_source_record_only_xyz_and_creation_id_changed": True, "complete_native_equals_independent_plan": True}


def check_unit_query(rows, wire):
    require(len(rows) == wire["count"], "HTTP unit count differs from independent native table")
    expected_ids = {"rawcode", "owner", "x", "y", "z", "facing", "variation", "hit_points", "mana",
                    "gold", "hero_level", "creation_id"}
    for actual, native in zip(rows, wire["rows"]):
        try:
            rawcode_text = native["rawcode"].decode("utf-8")
        except UnicodeDecodeError:
            rawcode_text = None
        require(actual["source_index"] == str(native["index"]) and actual["row_key"] == native["row_key"] and
                actual.get("rawcode_hex") == native["rawcode"].hex() and actual.get("creation_number") == native["creation_id"] and
                actual.get("rawcode_utf8") == rawcode_text and
                actual.get("owner") == native["owner"] and actual.get("position_world") == native["position"] and
                actual.get("position_world_raw_u32") == [struct.unpack("<I", struct.pack("<f", value))[0] for value in native["position"]],
                "HTTP typed unit locator/rawcode/creation/owner/XYZ differs from native source")
        table = fields(actual)
        require(set(table) == expected_ids, "actual unit field catalogue differs from the established typed row")
        for key in expected_ids:
            field = table[key]
            value = native["position"][("x", "y", "z").index(key)] if key in ("x", "y", "z") else native[key]
            raw = "hex:" + value.hex() if key == "rawcode" else raw_f32(value) if key in ("x", "y", "z", "facing") else str(value)
            kind = "bytes" if key == "rawcode" else "angle" if key == "facing" else "float32" if key in ("x", "y", "z") else "int32"
            default = key in ("hit_points", "mana") and value == -1
            display = None if default else rawcode_text if key == "rawcode" else value * 180.0 / math.pi if key == "facing" else value
            require(field["type"] == kind and field["raw"] == raw and field["is_default"] is default and
                    field["value"] == display and field["writable"] is (key in ("owner", "facing", "hit_points", "mana")),
                    f"HTTP unit {key} typed raw/value/default/writeability differs from native")


def expected_copy_rows(before_rows, source_index, wire):
    native = wire["rows"][-1]
    result = copy.deepcopy(before_rows)
    added = copy.deepcopy(before_rows[source_index])
    added.update({"source_index": str(native["index"]), "row_key": native["row_key"],
                  "creation_number": native["creation_id"], "position_world": native["position"],
                  "position_world_raw_u32": [struct.unpack("<I", struct.pack("<f", value))[0] for value in native["position"]]})
    for field in added["fields"]:
        if field["id"] in ("x", "y", "z"):
            value = native["position"][("x", "y", "z").index(field["id"])]
            field.update({"raw": raw_f32(value), "value": value})
        elif field["id"] == "creation_id":
            field.update({"raw": str(native["creation_id"]), "value": native["creation_id"]})
    result.append(added)
    return result


def expected_rotation_rows(original_rows, index, facing):
    result = copy.deepcopy(original_rows)
    for field in result[index]["fields"]:
        if field["id"] == "facing":
            field.update({"raw": raw_f32(f32(facing)), "value": f32(facing) * 180.0 / math.pi})
    return result


class PlacementHttpServer(EntityHttpServer):
    def start(self):
        super().start()
        listed = json.loads((self.evidence.output / f"{self.label}.tools-list.json").read_text(encoding="utf-8"))
        definitions = {tool.get("name"): tool for tool in listed.get("tools", [])}
        require(PLACEMENT_TOOLS <= definitions.keys(), "actual C2 placement tools are missing")
        common = {"kind", "project_identity", "expected_revision", "expected_member_sha256"}
        for name in PLACEMENT_TOOLS:
            schema = definitions[name].get("inputSchema", {})
            expected = common | ({"sources", "offset"} if name == "entities_copy" else {"source", "facing_radians"})
            require(schema.get("type") == "object" and set(schema.get("required", [])) == expected,
                    f"{name} actual required schema differs from the frozen C2 contract")
            props = schema.get("properties", {})
            require(set(props.get("kind", {}).get("enum", [])) == {"units", "doodads"} and
                    props.get("expected_revision", {}).get("type") == "string", "C2 categories/CAS contract is invalid")
            locator = props.get("sources", {}).get("items", {}) if name == "entities_copy" else props.get("source", {})
            require(locator.get("type") == "object" and set(locator.get("required", [])) == {"source_index", "row_key"} and
                    locator.get("properties", {}).get("source_index", {}).get("type") == "string",
                    "C2 source locator must be a snapshot index string plus exact row_key")
            if name == "entities_copy":
                require(props.get("sources", {}).get("minItems") == 1 and props.get("sources", {}).get("maxItems") == 32 and
                        set(props.get("offset", {}).get("required", [])) == {"x", "y", "z"} and
                        all(props.get("offset", {}).get("properties", {}).get(axis, {}).get("type") == "number" for axis in ("x", "y", "z")),
                        "C2 copy lacks the actual bounded sources/numeric XYZ schema")
            else:
                require(props.get("facing_radians", {}).get("type") == "number", "C2 rotate input is not numeric radians")
        self.evidence.check(f"{self.label}-actual-c2-tool-schemas", {"names": sorted(PLACEMENT_TOOLS),
            "schema_sha256": json_sha({name: definitions[name] for name in sorted(PLACEMENT_TOOLS)})})
        return self


def source_locator(row):
    return {"source_index": row["source_index"], "row_key": row["row_key"]}


def placement_request(reply, row, operation, value):
    result = {**context(reply), "kind": "units", "expected_member_sha256": reply["member"]["member_sha256"]}
    if operation == "copy":
        result.update({"sources": [source_locator(row)], "offset": dict(zip(("x", "y", "z"), value))})
    else:
        result.update({"source": source_locator(row), "facing_radians": value})
    return result


def begin(server):
    revision = server.tool("get_revision", {})
    require(revision.get("identity_checked") is True and revision.get("identity_current") is True, "source environment is not exact")
    started = server.tool("begin_edit", {"expected_revision": revision["revision"]})
    require(isinstance(started.get("transaction_id"), str) and isinstance(started.get("base_revision"), dict),
            "original transaction identity/base revision is missing")
    server.envelope = {"transaction_id": started["transaction_id"], "expected_revision": started["base_revision"]}
    return copy.deepcopy(server.envelope)


def reject_unchanged(server, evidence, label, name, request, code, rows, reply):
    envelope = copy.deepcopy(server.envelope)
    rejected = server.rejection(name, request, code)
    current_rows, current_reply = query_entities(server, "units")
    require(current_rows == rows and current_reply == reply and server.envelope == envelope,
            "refused placement changed full current units/binding/member SHA/CAS or original transaction")
    evidence.check(label, {"refusal": rejected, "current_overlay_revision_and_transaction_exact": True})


def reject_schema_unchanged(server, evidence, label, name, request, message, rows, reply):
    envelope = copy.deepcopy(server.envelope)
    revision = server.tool("get_revision", {})
    baseline = {kind: query_entities(server, kind) for kind in ENTITY_MEMBERS}
    require(baseline["units"] == (rows, reply) and server.envelope == envelope,
            "schema refusal baseline no longer matches the exact known active native/table/CAS/transaction")
    request_id = server.sequence + 1
    expected = {"code": -32602, "message": message}
    rejected = server.request("tools/call", {"name": name, "arguments": request}, expected_rpc_error=expected)
    require(isinstance(rejected, dict) and set(rejected) == {"jsonrpc", "id", "error"} and
            rejected["jsonrpc"] == "2.0" and type(rejected["id"]) is int and rejected["id"] == request_id and
            isinstance(rejected["error"], dict) and rejected["error"] == expected and
            type(rejected["error"]["code"]) is int,
            "schema refusal was not the exact same-id JSON-RPC -32602 response without a business receipt")
    for kind, before in baseline.items():
        require(query_entities(server, kind) == before,
                "schema refusal changed the full current entity table/native SHA/binding/identity/CAS: " + kind)
    require(server.tool("get_revision", {}) == revision and server.envelope == envelope,
            "schema refusal changed the exact original project revision or transaction envelope")
    proof = {"rpc_response": rejected, "http_200_verified_by_shared_transport": True,
             "all_four_current_tables_native_shas_and_bindings_exact": True,
             "original_revision_and_transaction_exact": True,
             "current_snapshot_sha256": {kind: json_sha(before) for kind, before in baseline.items()}}
    evidence.artifact(label + ".actual-schema-refusal.json", {"request": request, "proof": proof})
    evidence.check(label, proof)


def assert_receipt(edited, operation, before_reply, before_row, after_reply, after_row):
    require(edited.get("changed") is True and edited.get("saved") is False and edited.get("runtime_verified") is False and
            edited.get("kind") == "units", "placement receipt has invalid changed/kind/evidence claims")
    require(edited.get("before") == {"member": before_reply["member"], "rows": [before_row]} and
            edited.get("after") == {"member": after_reply["member"], "rows": [after_row]} and
            edited.get("before_binding") == before_reply["binding"], "placement full before/after receipt differs from real typed rows")
    command = {key: str(unsigned(edited.get(key), key, key in ("session_identity", "command_identity")))
               for key in ("session_identity", "command_identity", "session_revision_before", "session_revision_after")}
    require(int(command["session_revision_after"]) == int(command["session_revision_before"]) + 1 and
            context(edited) == context(after_reply) and context(before_reply)["project_identity"] == context(after_reply)["project_identity"] and
            context(before_reply)["expected_revision"] != context(after_reply)["expected_revision"], "placement actual receipt/CAS did not advance exactly once")
    require(isinstance(edited.get("changes"), dict), "placement full host changes receipt is missing")
    require(edited.get("operation") == operation and unsigned(edited.get("applied_placements"), "applied_placements") == 1 and
            edited.get("position_unit") == "game-world" and edited.get("rotation_unit") == "radians" and
            edited.get("field_display_angle_unit") == "degrees", "placement count/operation/unit receipt is incorrect")
    expected_ids = [{"kind": "units", **source_locator(after_row), "member_sha256": after_reply["member"]["member_sha256"]}] if operation == "copy" else []
    require(edited.get("new_ids") == expected_ids, "copy new_ids do not bind the actual new row/member or rotate fabricated IDs")
    return command


def restore_check(server, evidence, original, label):
    server.rollback()
    for kind in ENTITY_MEMBERS:
        rows, reply = query_entities(server, kind)
        require(rows == original[kind]["rows"] and reply["member"] == original[kind]["reply"]["member"],
                f"original rollback did not restore exact {kind} source rows/member")
    evidence.check(label, {"all_four_source_tables_and_native_member_shas_exact": True})


def inputs():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("server", "cli", "source", "workspace", "ydwe-root", "war3-root", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--qt-bin", type=Path)
    parser.add_argument("--expected-source-sha256", required=True)
    for name, default in (("start-timeout", 60), ("request-timeout", 60), ("save-timeout", 900), ("cli-timeout", 60)):
        parser.add_argument("--" + name, type=float, default=default)
    args = parser.parse_args()
    require(HEX_SHA.fullmatch(args.expected_source_sha256), "expected source archive SHA must be 64 lowercase hex")
    for name in ("start_timeout", "request_timeout", "save_timeout", "cli_timeout"):
        require(0 < getattr(args, name) <= 3600, "timeouts must be positive and no greater than one hour")
    args.output = args.output.resolve()
    require(not args.output.is_relative_to(args.workspace.resolve()), "output must be outside source workspace")
    args.output.mkdir(parents=True, exist_ok=False)
    return args


def exercise_rotation(server, evidence, original, native, selected, offset):
    envelope = begin(server)
    rows, reply = query_entities(server, "units")
    commands = []
    first_request = None
    for facing in (1.125, 1.5):
        # 这两个精确 float 经原 Move 的 nested fmodf 规范化仍逐位不变。
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
                edited.get("requested_facing_radians") == f32(facing),
                "absolute rotation changed another field/row or lost the requested float/original transaction")
        command = assert_receipt(edited, "rotate", before_reply, before_rows[selected["index"]], reply, rows[selected["index"]])
        commands.append(command)
        evidence.artifact(f"rotation-{len(commands)}.actual-http.json", {"request": request, "write": edited, "rows": rows, "reply": reply})
        evidence.check(f"absolute-rotation-{len(commands)}-bound-to-source-plus-only-f32-facing-slot", {
            "requested_radians": facing, "requested_raw": raw_f32(facing), "command": command,
            "source_sha256": native["sha256"], "expected_overlay_sha256": expected["sha256"],
            "source_facing_offset": selected["start"] + 20, "saved": False})
    require((commands[0]["session_identity"], commands[0]["command_identity"]) !=
            (commands[1]["session_identity"], commands[1]["command_identity"]), "repeated rotation reused the same actual receipt tuple")
    copy_request = placement_request(reply, rows[selected["index"]], "copy", offset)
    reject_unchanged(server, evidence, "rotation-then-copy-refused-before-publication", "entities_copy", copy_request,
                     "MCP_PLACEMENT_COMBINATION_UNSUPPORTED", rows, reply)
    reject_unchanged(server, evidence, "rotation-stale-cas-refused", "entities_rotate", first_request,
                     "ENTITY_REVISION_CONFLICT", rows, reply)
    for label, facing, code in (("negative-radians", -0.25, "MAP_PLACEMENT_FACING_INVALID"),
                                ("above-turn-radians", 8.0, "MAP_PLACEMENT_FACING_INVALID")):
        reject_unchanged(server, evidence, label + "-refused-current-rotation-preserved", "entities_rotate",
                         placement_request(reply, rows[selected["index"]], "rotate", facing), code, rows, reply)
    reject_schema_unchanged(server, evidence, "string-radians-schema-refused-current-rotation-preserved", "entities_rotate",
        placement_request(reply, rows[selected["index"]], "rotate", "1.125"),
        "arguments.facing_radians must be a finite number", rows, reply)
    other = next((row for row in rows if row["source_index"] != str(selected["index"])), None)
    require(other is not None, "different-original-row rotation refusal needs a second real unit")
    reject_unchanged(server, evidence, "different-original-row-rotation-refused", "entities_rotate",
                     placement_request(reply, other, "rotate", 0.5), "MCP_PLACEMENT_COMBINATION_UNSUPPORTED", rows, reply)
    for kind in ("doodads", "regions", "cameras"):
        current, described = query_entities(server, kind)
        require(current == original[kind]["rows"] and described["member"] == original[kind]["reply"]["member"],
                "rotation altered another native entity category")
    restore_check(server, evidence, original, "rotation-transaction-rollback-restores-every-source-table")
    evidence.receipt["rotation"] = {"two_absolute_commands_verified": True, "last_requested_radians": 1.5,
        "last_native_facing_raw": raw_f32(1.5), "source_byte_replay_verified_by_overlay_sha": True,
        "rollback_verified": True, "candidate_saved": False, "saved_rotation_verified": False}


def exercise_fields_then_placement(server, evidence, original, native, selected, offset):
    begin(server)
    rows, reply = query_entities(server, "units")
    row = rows[selected["index"]]
    owner = selected["owner"] + 1 if selected["owner"] < 15 else 0
    require(0 <= selected["owner"] <= 15 and owner != selected["owner"], "source has no legal distinct C1 owner test")
    edited = server.tool("entities_set_fields", {**context(reply), "kind": "units",
        "expected_member_sha256": reply["member"]["member_sha256"], "changes": [change(row, "owner", owner)]})
    current_rows, current_reply = query_entities(server, "units")
    expected = bytearray(native["data"])
    struct.pack_into("<i", expected, selected["start"] + 37, owner)
    field_wire = read_units_bytes(bytes(expected))
    check_unit_query(current_rows, field_wire)
    expected_rows = copy.deepcopy(rows)
    expected_rows[selected["index"]]["owner"] = owner
    for field in expected_rows[selected["index"]]["fields"]:
        if field["id"] == "owner":
            field.update({"raw": str(owner), "value": owner})
    require(current_rows == expected_rows and current_reply["member"]["member_sha256"] == field_wire["sha256"] and
            edited.get("changed") is True and unsigned(edited.get("applied_fields"), "applied_fields") == 1,
            "actual C1 source-field precondition did not produce the independently exact owner delta")
    for operation, value in (("copy", offset), ("rotate", 0.5)):
        reject_unchanged(server, evidence, f"existing-c1-owner-field-then-{operation}-refused", "entities_" + operation,
            placement_request(current_reply, current_rows[selected["index"]], operation, value),
            "MCP_PLACEMENT_MIXED_FIELDS_UNSUPPORTED", current_rows, current_reply)
    restore_check(server, evidence, original, "c1-mixed-field-precondition-rolled-back-exactly")


def restore_region_noop_overlay(server, evidence, original, native, envelope):
    rows, reply = query_entities(server, "regions")
    check_region_query(rows, native)
    require(rows == original["regions"]["rows"] and reply["member"] == original["regions"]["reply"]["member"],
            "net-zero region precondition differs from the independently extracted original table")
    selected = None
    for item in native["rows"]:
        left, right = item["bounds"]["left"], item["bounds"]["right"]
        shifted = struct.unpack("<f", struct.pack("<f", left + 1.0))[0]
        if math.isfinite(shifted) and abs(shifted) <= 1000000 and shifted != left and shifted < right:
            selected = item, shifted
            break
    require(selected is not None, "net-zero regression needs a real region with a legal distinct left+1 float")
    item, shifted = selected
    index, left = item["index"], item["bounds"]["left"]
    changed_bytes = bytearray(native["data"])
    struct.pack_into("<f", changed_bytes, item["start"], shifted)
    changed_wire = read_regions_bytes(bytes(changed_bytes))
    expected_rows = copy.deepcopy(rows)
    for field in expected_rows[index]["fields"]:
        if field["id"] == "left":
            field.update({"raw": raw_f32(shifted), "value": shifted})
    commands = []
    for value, wire, expected_rows in ((shifted, changed_wire, expected_rows),
                                       (left, native, original["regions"]["rows"])):
        before = reply
        edited = server.tool("entities_set_fields", {**context(reply), "kind": "regions",
            "expected_member_sha256": reply["member"]["member_sha256"],
            "changes": [change(rows[index], "left", value)]})
        rows, reply = query_entities(server, "regions")
        check_region_query(rows, wire)
        require(rows == expected_rows and reply["member"]["member_sha256"] == wire["sha256"] and
                edited.get("changed") is True and unsigned(edited.get("applied_fields"), "applied_fields") == 1 and
                context(reply)["project_identity"] == context(before)["project_identity"] and
                context(reply)["expected_revision"] != context(before)["expected_revision"] and server.envelope == envelope,
                "real same-transaction region delta/restore failed the exact native-byte/table/CAS proof")
        commands.append({"requested_left_raw": raw_f32(value), "member_sha256": wire["sha256"], "write": edited})
    require(reply["member"] == original["regions"]["reply"]["member"],
            "restored W3R snapshot is not the original independently bound member")
    evidence.artifact("copy.region-original-byte-noop-overlay.json", {"commands": commands, "rows": rows, "reply": reply})
    evidence.check("original-byte-noop-overlay-preserved", {"same_original_transaction": True,
        "source_index": str(index), "original_member_sha256": native["sha256"],
        "changed_member_sha256": changed_wire["sha256"], "restored_member_sha256": reply["member"]["member_sha256"],
        "original_rows_exact": True, "two_real_c1_writes": True})


def copy_refusals(server, evidence, original, selected, offset, rows, reply, initial_request):
    row = rows[selected["index"]]
    request = placement_request(reply, row, "copy", offset)
    reject_unchanged(server, evidence, "copy-stale-opaque-cas-refused-current-clone-preserved", "entities_copy",
                     initial_request, "ENTITY_REVISION_CONFLICT", rows, reply)
    bad = copy.deepcopy(request); bad["project_identity"] += "-stale"
    reject_unchanged(server, evidence, "copy-wrong-project-identity-refused", "entities_copy", bad,
                     "ENTITY_REVISION_CONFLICT", rows, reply)
    bad = copy.deepcopy(request); bad["expected_member_sha256"] = "0" * 64
    reject_unchanged(server, evidence, "copy-wrong-native-member-sha-refused", "entities_copy", bad,
                     "MAP_PLACEMENT_MEMBER_STALE", rows, reply)
    bad = copy.deepcopy(request); bad["sources"][0]["row_key"] += "00"
    reject_unchanged(server, evidence, "copy-wrong-source-row-locator-refused", "entities_copy", bad,
                     "MAP_PLACEMENT_ROW_STALE", rows, reply)
    bad = copy.deepcopy(request); bad["sources"][0]["source_index"] = str(len(rows))
    reject_unchanged(server, evidence, "copy-out-of-range-source-refused", "entities_copy", bad,
                     "ENTITY_SOURCE_INDEX_INVALID", rows, reply)
    bad = copy.deepcopy(request); bad["sources"][0]["source_index"] = "00"
    reject_unchanged(server, evidence, "copy-noncanonical-source-index-refused", "entities_copy", bad,
                     "ENTITY_INDEX_INVALID", rows, reply)
    bad = copy.deepcopy(request); bad["offset"]["x"] = 1000001
    reject_unchanged(server, evidence, "copy-world-offset-outside-domain-refused", "entities_copy", bad,
                     "PLACEMENT_COORDINATE_INVALID", rows, reply)
    for axis in ("x", "y"):
        bad = copy.deepcopy(request); bad["offset"][axis] = "0"
        reject_schema_unchanged(server, evidence, "copy-nonnumeric-world-offset-" + axis + "-schema-refused", "entities_copy", bad,
                               "arguments.offset." + axis + " must be a finite number", rows, reply)
    bad = copy.deepcopy(request); bad["kind"] = "regions"
    reject_schema_unchanged(server, evidence, "copy-wrong-entity-kind-schema-refused", "entities_copy", bad,
                           "arguments.kind is not an allowed value", rows, reply)
    bad = copy.deepcopy(request); bad["sources"] *= 33
    reject_unchanged(server, evidence, "copy-more-than-32-sources-refused", "entities_copy", bad,
                     "MAP_PLACEMENT_LIMIT", rows, reply)
    for operation, value in (("copy", offset), ("rotate", 0.5)):
        reject_unchanged(server, evidence, f"existing-copy-then-{operation}-refused-before-publication", "entities_" + operation,
                         placement_request(reply, row, operation, value), "MCP_PLACEMENT_COMBINATION_UNSUPPORTED", rows, reply)
    region_rows, region_reply = query_entities(server, "regions")
    region = next((item for item in region_rows if fields(item)["left"]["value"] + 1 < fields(item)["right"]["value"]), None)
    require(region is not None, "placement/field mixing refusal requires one actual region with room for a legal left bound")
    requested = {**context(region_reply), "kind": "regions", "expected_member_sha256": region_reply["member"]["member_sha256"],
                 "changes": [change(region, "left", fields(region)["left"]["value"] + 1)]}
    reject_unchanged(server, evidence, "existing-copy-then-legal-region-field-refused-before-publication", "entities_set_fields",
                     requested, "MCP_PLACEMENT_MIXED_FIELDS_UNSUPPORTED", rows, reply)
    actual_regions, actual_reply = query_entities(server, "regions")
    require(actual_regions == region_rows and actual_reply == region_reply, "refused mixed region write changed its original table/revision")
    for kind in ("doodads", "regions", "cameras"):
        current, described = query_entities(server, kind)
        require(current == original[kind]["rows"] and described["member"] == original[kind]["reply"]["member"],
                "copy/refusals altered another native entity category")


def finish_candidate(args, env, evidence, servers, server, source_before, source_members, source_audit,
                     original, native, expected, selected, offset, after_rows):
    preview = server.tool("preview_edit", server.envelope)
    evidence.artifact("copy.transaction.preview.json", preview)
    patches = preview.get("changes")
    require(isinstance(patches, list) and len(patches) == 2 and all(isinstance(item, dict) and item.get("kind") == "map-data"
            for item in patches), "copy preview must retain exactly two native patches including the original-byte W3R overlay")
    bound = {normalized_member(item.get("path", "")): item for item in patches}
    region_sha = source_audit["war3map.w3r"]["sha256"]
    require(set(bound) == {UNIT_MEMBER, "war3map.w3r"} and
            bound[UNIT_MEMBER].get("before_sha256") == native["sha256"] and
            bound[UNIT_MEMBER].get("after_sha256") == expected["sha256"] and
            bound["war3map.w3r"].get("before_sha256") == region_sha and
            bound["war3map.w3r"].get("after_sha256") == region_sha,
            "copy preview differs from the single exact unit delta plus independently source-bound zero-delta W3R overlay")
    validated = server.tool("validate_edit", server.envelope)
    require(validated.get("satisfied") is True and validated.get("highest_proven_level", -1) >= 0,
            "current exact copy generation did not validate through the original transaction")
    evidence.check("original-copy-transaction-preview-and-validation-bound-to-independent-native-plan", {"preview": preview, "validation": validated})
    candidate = args.output / ("mcp-placements-c2-copy-candidate" + args.source.suffix.lower())
    require(not candidate.exists(), "candidate exists")
    saved = server.tool("save_map_edit_candidate", {"source_map_path": str(args.source), "output_path": str(candidate),
        "expected_source_sha256": source_before, **server.envelope, "script_target": "source"}, timeout=args.save_timeout)
    require(candidate.is_file() and saved.get("candidate_kind") == "mpq-map-archive" and
            saved.get("source_sha256") == source_before and saved.get("candidate_sha256") == sha(candidate) and
            saved.get("script_rebuilt") is True and saved.get("native_editor_verified") is False and
            saved.get("game_runtime_verified") is False and isinstance(saved.get("output_path"), str) and
            Path(saved["output_path"]).resolve(strict=True) == candidate, "copy save archive identity/evidence claims are invalid")
    for key, count in (("applied_placement_count", 1), ("applied_field_edit_count", 0), ("applied_terrain_stroke_count", 0)):
        require(type(saved.get(key)) is int and saved[key] == count, "copy save did not consume the exact placement-only command count")
    edited = saved.get("edited_members")
    require(isinstance(edited, list) and len(edited) == 1 and normalized_member(edited[0].get("name", "")) == UNIT_MEMBER and
            edited[0].get("original_sha256") == native["sha256"] and edited[0].get("saved_sha256") == expected["sha256"],
            "copy save edited-member receipt is not exactly the independently bound unit native patch")
    trigger = saved.get("trigger_transaction_receipt")
    if trigger is not None:
        require(isinstance(trigger, dict) and trigger.get("transaction_id") == server.envelope["transaction_id"] and
                trigger.get("transaction_active") is True and trigger.get("workspace_rebound") is False,
                "copy save changed the original transaction identity or rebound its workspace")
    evidence.receipt.update({"saved": saved, "candidate": str(candidate)})
    evidence.check("original-save-service-produced-real-single-copy-candidate", saved)
    restore_check(server, evidence, original, "saved-copy-source-overlay-rollback-restores-all-source-tables")
    server.close()

    members = list_members(args, env, evidence, candidate, "candidate")
    workspace = args.output / "candidate-workspace"
    manifest = extract_members(args, env, evidence, candidate, list(members.values()), workspace, "candidate")
    require(UNIT_MEMBER in manifest and manifest[UNIT_MEMBER]["sha256"] == expected["sha256"],
            "CLI-extracted saved unit member differs from current copy/save receipt/native plan")
    protected = {}
    for name in PROTECTED_MEMBERS:
        before, after = source_audit.get(name), manifest.get(name)
        require((before is None) == (after is None) and (before is None or before["sha256"] == after["sha256"]),
                "protected member presence or bytes changed: " + name)
        protected[name] = {"present": before is not None, "source_sha256": before["sha256"] if before else None,
                           "candidate_sha256": after["sha256"] if after else None, "body_inspected": False}
    evidence.check("trigger-pair-seven-object-members-other-native-and-tiara-presence-and-sha-preserved", protected)
    actual_native = read_units(workspace / safe_member(members[UNIT_MEMBER]))
    proof = copy_proof(native, actual_native, selected, offset)
    evidence.artifact("copy.candidate.independent-byte-proof.json", proof)
    evidence.check("entire-saved-native-equals-header-count-plus-exact-source-slice-copy-oracle", proof)

    reopened = PlacementHttpServer(args, env, workspace, "candidate", evidence)
    servers.append(reopened)
    reopened.start()
    rows, reply = query_entities(reopened, "units")
    check_unit_query(rows, actual_native)
    require(rows == after_rows and reply["member"]["member_sha256"] == manifest[UNIT_MEMBER]["sha256"],
            "second independent HTTP service did not reopen the exact entire saved unit table/new locator")
    for kind in ("doodads", "regions", "cameras"):
        current, described = query_entities(reopened, kind)
        require(current == original[kind]["rows"] and described["member"] == original[kind]["reply"]["member"],
                "saved candidate reopen lost/altered another source entity category")
    require(reopened.record["initial_revision"]["revision"]["ui_catalog_sha256"] ==
            server.record["initial_revision"]["revision"]["ui_catalog_sha256"], "saved candidate reopened against another actual catalog")
    evidence.artifact("copy.candidate.actual-http-readback.json", {"rows": rows, "reply": reply})
    evidence.check("second-owned-http-service-exact-saved-copy-and-other-entity-readback", {
        "source_archive_sha256": source_before, "candidate_archive_sha256": sha(candidate),
        "candidate_directory_aggregate_sha256": reply["binding"]["source_sha256"],
        "native_member_sha256": manifest[UNIT_MEMBER]["sha256"], "new_row": source_locator(rows[-1]),
        "native_rawcode_hex": rows[-1]["rawcode_hex"], "creation_number": rows[-1]["creation_number"],
        "snapshot_row_requeried": True, "permanent_ID_claimed": False})
    reopened.close()


def main():
    args = inputs()
    evidence = Evidence(args.output)
    evidence.receipt.update({"schema": "audastra-mcp-placements-c2-http-probe-v1", "scope": "single-existing-unit-copy-save-and-two-absolute-rotations-rollback",
        "whole_C_group_verified": False, "row_locators_across_revisions_claimed_permanent": False, "saved_rotation_verified": False})
    servers, source_before, workspace_before = [], None, None
    try:
        env, source_before, workspace_before = prepare(args, evidence)
        members = list_members(args, env, evidence, args.source, "source")
        require({UNIT_MEMBER, "war3map.wtg", "war3map.wct", "war3map.w3r"} <= members.keys(),
                "source lacks required native unit/region/trigger sources")
        names = [members[name] for name in (UNIT_MEMBER,) + PROTECTED_MEMBERS if name in members]
        audit_dir = args.output / "source-member-hashes"
        audit = extract_members(args, env, evidence, args.source, names, audit_dir, "source-audit")
        for name, bound in audit.items():
            path = args.workspace / safe_member(members[name])
            require(path.is_file() and sha(path) == bound["sha256"], "input workspace/source archive member mismatch: " + name)
        native = read_units(audit_dir / safe_member(members[UNIT_MEMBER]))
        region_native = read_wire(audit_dir / safe_member(members["war3map.w3r"]), read_regions_bytes)
        offset = [32.0, -16.0, 8.0]
        selected = next((row for row in native["rows"] if row["rawcode"] != b"sloc" and
                         0 <= row["owner"] <= 15 and all(abs(value) < 900000 for value in row["position"]) and
                         all(abs(value) <= 1000 for value in row["scale"]) and row["facing"] not in (1.125, 1.5)), None)
        require(selected is not None, "source has no legal existing non-start unit for the exact copy/rotation probe")
        expected = read_units_bytes(expected_copy_bytes(native, selected, offset))
        evidence.artifact("copy.requested.independent-native-plan.json", copy_proof(native, expected, selected, offset))
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
        require(original["units"]["reply"]["binding"]["source_sha256"] == server.record["initial_revision"]["revision"]["source_sha256"],
                "expanded-source aggregate is confused with source archive SHA")
        evidence.check("actual-complete-source-unit-table-and-independent-classic-variable-record-wire-binding", {
            "unit_count": native["count"], "nested_record_count": native["nested_records"], "native_member_sha256": native["sha256"],
            "source_archive_sha256": source_before, "directory_aggregate_sha256": original["units"]["reply"]["binding"]["source_sha256"],
            "selected_source_index": str(selected["index"]), "selected_rawcode_hex": selected["rawcode"].hex(),
            "layout": "classic-v8-sub11-unambiguous", "new_cpp_copy_logic_used_by_oracle": False})
        exercise_rotation(server, evidence, original, native, selected, offset)
        exercise_fields_then_placement(server, evidence, original, native, selected, offset)

        envelope = begin(server)
        restore_region_noop_overlay(server, evidence, original, region_native, envelope)
        rows, reply = query_entities(server, "units")
        sloc = next((row for row in rows if row["rawcode_hex"] == b"sloc".hex()), None)
        require(sloc is not None, "source has no real starting-location refusal fixture")
        reject_unchanged(server, evidence, "actual-start-location-copy-refused", "entities_copy",
                         placement_request(reply, sloc, "copy", offset), "PLACEMENT_START_COPY_UNSUPPORTED", rows, reply)
        request = placement_request(reply, rows[selected["index"]], "copy", offset)
        duplicated = copy.deepcopy(request); duplicated["sources"] *= 2
        reject_unchanged(server, evidence, "duplicate-real-source-in-one-copy-batch-refused", "entities_copy",
                         duplicated, "PLACEMENT_DUPLICATE", rows, reply)
        before_rows, before_reply = rows, reply
        edited = server.tool("entities_copy", request)
        rows, reply = query_entities(server, "units")
        check_unit_query(rows, expected)
        require(rows == expected_copy_rows(before_rows, selected["index"], expected) and
                reply["member"]["member_sha256"] == expected["sha256"] and server.envelope == envelope,
                "copy changed original typed rows or failed the exact appended source record/SHA/transaction proof")
        command = assert_receipt(edited, "copy", before_reply, before_rows[selected["index"]], reply, rows[-1])
        evidence.artifact("copy.active.actual-http.json", {"request": request, "write": edited, "rows": rows, "reply": reply})
        evidence.check("actual-single-unit-copy-published-once-with-complete-source-new-row-receipt", {
            "command": command, "new_row": source_locator(rows[-1]), "new_creation_id": rows[-1]["creation_number"],
            "new_rawcode_hex": rows[-1]["rawcode_hex"], "expected_native_sha256": expected["sha256"]})
        copy_refusals(server, evidence, original, selected, offset, rows, reply, request)
        finish_candidate(args, env, evidence, servers, server, source_before, members, audit,
                         original, native, expected, selected, offset, rows)
        require(sha(args.source) == source_before and inventory(args.workspace) == workspace_before and
                not evidence.receipt["cleanup_errors"], "source/workspace identity or owned cleanup failed")
        evidence.receipt["passed"] = True
        evidence.checkpoint("passed-independent-c2-copy-http-and-archive-readback")
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
                current = inventory(args.workspace)
                evidence.artifact("source-workspace.after.json", current)
                evidence.receipt.update({"workspace_inventory_sha256_after": json_sha(current), "source_workspace_unchanged": current == workspace_before})
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
