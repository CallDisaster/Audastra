"""P1 连续往返的实际探针适配器：共用Qt保存、CLI独立分页回读；不运行游戏。"""
from __future__ import annotations

import argparse
import importlib.util
import json
import math
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
_spec = importlib.util.spec_from_file_location("p1_support", Path(__file__).with_name("p1-support.py"))
support = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(support)

KINDS = ("settings", "units", "doodads", "regions", "cameras", "sounds", "strings")


def semantic_state(cli, source, query=None):
    source_sha = support.sha256(source)
    query = query or (lambda kind, offset: support.command_json(
        [str(cli), "map-data-query", str(source), source_sha, kind, str(offset), "128"]))
    first = query("settings", 0)
    support.require(first.get("ok") is True and first.get("binding", {}).get("source_sha256") == source_sha,
                    "地图数据查询没有绑定实际来源SHA")
    members = {row["kind"]: row for row in first["members"]}
    support.require(set(KINDS) <= members.keys(), "CLI 缺地图数据成员状态")
    state = {}
    for kind in KINDS:
        member = members[kind]
        support.require(member.get("state") in ("ready", "missing"), f"{kind} 未完整读取，不能连续验收")
        state[kind + "/member_state"] = member["state"]
        if member["state"] == "missing":
            continue
        count = int(member["record_count"])
        support.require(0 <= count <= 16384, "记录数量超过本脚本有界预算16384")
        state[kind + "/record_count"] = count
        offset = 0
        for page_number in range(128):
            reply = first if kind == "settings" and page_number == 0 else query(kind, offset)
            support.require(reply.get("ok") is True and reply.get("binding", {}).get("source_sha256") == source_sha and
                            reply.get("member", {}).get("kind") == kind and reply["member"].get("state") == "ready",
                            "分页查询身份/类别/状态变化")
            rows = reply.get("rows")
            support.require(isinstance(rows, list) and len(rows) <= 128, "分页行数超预算")
            for row in rows:
                index = int(row["source_index"])
                support.require(index == offset, "分页源索引不连续")
                prefix = f"{kind}/{index}/"
                fields = row.get("fields")
                support.require(isinstance(fields, list), "查询没有字段数组")
                for field in fields:
                    key = prefix + field["id"]
                    support.require(key not in state and isinstance(field.get("raw"), str), "字段重复或没有raw身份")
                    state[key] = field["raw"]
                if not fields:
                    # 只读行（字符串/地物）的公开语义；不混入snapshot修订或row_key。
                    state[prefix + "readonly_row"] = {key: row.get(key) for key in
                        ("form", "rawcode_hex", "creation_number", "string_id", "owner", "position_world_raw_u32", "resource_path_hex")}
                    if row.get("name"):
                        state[prefix + "readonly_row"]["name_raw_hex"] = row["name"]["raw_hex"]
                offset += 1
            cursor = reply.get("next_cursor")
            if cursor is None:
                support.require(offset == count, "分页总行数不符")
                break
            support.require(rows and int(cursor["next_index"]) == offset, "分页游标没有前进")
        else:
            raise ValueError("分页超过128页预算")
    support.require(support.sha256(source) == source_sha, "来源在分页期间变化")
    return source_sha, state


def float_add(raw, delta):
    support.require(isinstance(raw, str) and raw.startswith("f32:") and len(raw) == 12, "预期float32原始字段")
    number = struct.unpack(">f", bytes.fromhex(raw[4:]))[0]
    support.require(math.isfinite(number), "非有限float32不能做探针修改")
    result = struct.pack(">f", number + delta)
    support.require(math.isfinite(struct.unpack(">f", result)[0]), "修改后float32溢出")
    return "f32:" + result.hex()


def predicted_changes(state):
    # 与既有 ProbeMapDataEditAndSave 完全同一修改合同；保存前计算预期。
    support.require(state.get("settings/record_count") == 1 and state.get("units/record_count", 0) > 0,
                    "当前地图没有探针必需的设置或首个预放单位")
    changes = {"settings/0/map_name": "hex:" + b"FORGE_Map_Data_Probe".hex(),
               "units/0/x": float_add(state["units/0/x"], 64.0)}
    for kind, field, delta in (("regions", "right", 32.0), ("cameras", "distance", 100.0)):
        if state.get(kind + "/record_count", 0):
            key = f"{kind}/0/{field}"
            changes[key] = float_add(state[key], delta)
    if state.get("sounds/record_count", 0):
        volume = int(state["sounds/0/volume"])
        changes["sounds/0/volume"] = str(100 if volume < 0 else (volume - 10 if volume > 50 else volume + 10))
    return changes


def make_plan(editor, cli, game_root, ui_root):
    common = ["--editor", str(editor), "--cli", str(cli), "--game-root", str(game_root), "--ui-root", str(ui_root)]
    adapter = [sys.executable, str(Path(__file__).resolve())]
    return {"schema": "p1-roundtrip-plan-v1", "evidence_kind": "editor-cli",
            "coverage": ["公开地图数据raw字段及只读行保持；设置/单位X及实际存在的区域/镜头/声音探针修改"],
            "open": [*adapter, "open", "--input", "{input}", *common],
            "edit_save": [*adapter, "save", "--input", "{input}", "--output", "{output}", *common],
            "readback": [*adapter, "readback", "--input", "{output}", *common]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("plan", "open", "save", "readback"))
    parser.add_argument("--input", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--editor", required=True, type=Path)
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--game-root", required=True, type=Path)
    parser.add_argument("--ui-root", required=True, type=Path)
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    if args.action == "plan":
        support.require(args.output is not None, "plan 必须给 --output 新JSON路径")
        support.write_json(args.output, make_plan(args.editor, args.cli, args.game_root, args.ui_root))
        print(json.dumps({"status": "plan-written", "plan": str(args.output)}))
        return 0
    support.require(args.input is not None, "实际驱动必须给 --input")
    source = support.plain_path(args.input)
    input_sha = support.sha256(source)
    binaries = {name: support.sha256(support.plain_path(getattr(args, name))) for name in ("editor", "cli")}
    command = [str(args.editor), "--open-map", str(source), "--exit-after-open", "--game-root", str(args.game_root),
               "--ui-root", str(args.ui_root), "--print-summary"]
    if args.action == "save":
        support.require(args.output is not None and not args.output.exists(), "save 必须给一个不存在的输出路径")
        source, target = support.separate_paths(source, args.output)
        _, before = semantic_state(args.cli, source)
        changes = predicted_changes(before)
        command += ["--probe-map-data-save", str(target)]
        log = support.run_command(command, timeout=args.timeout)
        support.require("map-data-probe: data=true script=true" in log and target.is_file(), "实际Qt探针没有完成数据/脚本回读")
        result = {"input_sha256": input_sha, "output_sha256": support.sha256(target), "changes": changes,
                  "edited_fields": sorted(changes), "probe_log": log, "binary_sha256": binaries}
    else:
        support.run_command(command, timeout=args.timeout)
        source_sha, state = semantic_state(args.cli, source)
        result = {"source_sha256": source_sha, "state": state, "binary_sha256": binaries,
                  "coverage": "all published raw map-data fields and readonly row semantics; missing members explicit"}
    support.require(support.sha256(source) == input_sha, "Qt探针修改了只读来源")
    support.require(all(support.sha256(getattr(args, name)) == digest for name, digest in binaries.items()), "驱动期间二进制发生变化")
    print(json.dumps(result, ensure_ascii=False, allow_nan=False))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"P1 map-data driver: {error}", file=sys.stderr)
        sys.exit(2)
