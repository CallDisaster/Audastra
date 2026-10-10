#!/usr/bin/env python3
"""公共物编 facade 的有界候选合同。plan/verify 只读；exercise 只回滚，不保存。

实际 Save 由集成者调用原服务。apply_plan 可复用于持有同一自有 HTTP 实例的
root runner；本文件不解析/序列化物编二进制，也不改变旧 WP2 固定夹具。
"""
from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import math
import os
import struct
import sys
from pathlib import Path

_spec = importlib.util.spec_from_file_location("mcp_object_http_probe", Path(__file__).with_name("mcp_object_http_probe.py"))
probe = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(probe)
KINDS = probe.KINDS
MEMBERS = dict(zip(KINDS, probe.OBJECT_MEMBERS))
SCHEMA = "p1-public-object-matrix-v2"
PAGE_LIMIT = 128
MAX_FIELD_PAGES = 32
MAX_OPTION_PAGES = 64
READ_TOOLS = {"objects_list", "objects_get", "objects_field_options", "get_revision"}


class MatrixError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise MatrixError(message)


def full_key(row, kind=None):
    """经典扩展格式的 getter 省略 scalar 0；合同按类别恢复 wire 0。

    普通 W3U/T/H/B 没有扩展地址，保留 null；不把 data_index 当成 setter 参数。
    """
    return {"field": row["field"], "level": row.get("level", 0 if kind in ("ability", "upgrade") else None),
            "variation": row.get("variation", 0 if kind == "doodad" else None), "data_index": row.get("data_index")}


def public_raw(row):
    """getter 的精确原值投影，附可确定的 primitive 表示；不是 native record serializer。

    非空有效字符串列表可恢复原逗号文字。空数组折叠了 '_' 与 ''，bool 折叠了
    非零 int32，损坏 UTF-8 文字也会丢失 bytes；这些值不能冒称精确 raw。
    原生成员 SHA 是 wire bytes 身份，独立保存回放仍由 root 验收。
    """
    value = row.get("value")
    require(value is not None and type(value) is not bool, "RAW_PROJECTION_AMBIGUOUS: null/bool")
    if row.get("ordered_list"):
        require(isinstance(value, list) and 0 < len(value) <= 256, "RAW_PROJECTION_AMBIGUOUS: empty/list budget")
        require(all(isinstance(x, str) and x and not any(c in x for c in ",\r\n\0\ufffd") for x in value),
                "RAW_PROJECTION_AMBIGUOUS: invalid list text")
        text = ",".join(value)
        require(text != "_", "RAW_PROJECTION_AMBIGUOUS: collapsed sentinel")
        return {"kind": "string", "utf8_hex": text.encode("utf-8").hex(), "text": text}
    if row.get("type") in ("real", "unreal"):
        require(type(value) in (int, float) and math.isfinite(value), "RAW_PROJECTION_AMBIGUOUS: real")
        return {"kind": row["type"], "f32_hex": struct.pack(">f", value).hex()}
    if type(value) is int:
        require(-(2**31) <= value < 2**31, "RAW_PROJECTION_AMBIGUOUS: int32")
        return {"kind": "int32", "decimal": str(value)}
    if isinstance(value, str):
        require("\ufffd" not in value and "\0" not in value, "RAW_PROJECTION_AMBIGUOUS: undecodable text")
        return {"kind": "string", "utf8_hex": value.encode("utf-8").hex(), "text": value}
    raise MatrixError("RAW_PROJECTION_AMBIGUOUS: unsupported value")


def metadata(row):
    return {key: row.get(key) for key in ("field", "raw_key", "type", "minimum", "maximum", "ordered_list", "editable", "reason")}


def page_rows(server, tool, arguments, key, max_pages):
    rows, first = [], None
    for _ in range(max_pages):
        reply = server.tool(tool, {**arguments, "page": {"offset": len(rows), "limit": PAGE_LIMIT}})
        first = first or reply
        require(reply.get("project_identity") == first.get("project_identity") and reply.get("revision") == first.get("revision"),
                "QUERY_REVISION_CHANGED")
        batch = reply.get(key)
        require(isinstance(batch, list) and len(batch) <= PAGE_LIMIT, "QUERY_PAGE_INVALID")
        require(reply.get("offset") == len(rows) and reply.get("total") == first.get("total"), "QUERY_PAGINATION_CHANGED")
        rows.extend(batch)
        if not reply.get("truncated"):
            require(len(rows) == first.get("total"), "QUERY_INCOMPLETE")
            return {**first, key: rows}
        require(batch, "QUERY_NO_PROGRESS")
    raise MatrixError("QUERY_BUDGET_EXCEEDED")


def fields(server, kind, object_id, selected=None):
    args = {"kind": kind, "id": object_id}
    if selected is not None:
        args["fields"] = selected
    return page_rows(server, "objects_get", args, "fields", MAX_FIELD_PAGES)


def options(server, kind, object_id, field):
    return page_rows(server, "objects_field_options", {"kind": kind, "id": object_id, "field": field}, "items", MAX_OPTION_PAGES)


def address(row, all_fields, kind=None):
    key = full_key(row, kind)
    require(len(row["field"]) == 4, "FIELD_NOT_NATIVE_ID")
    require(not (key["level"] and key["variation"]), "FIELD_LEVEL_VARIATION_CONFLICT")
    # setter 没有 data 参数；同 field/level/variation 若存在多个 data 地址，必须拒绝。
    siblings = [f for f in all_fields if f["field"] == key["field"] and full_key(f, kind)["level"] == key["level"]
                and full_key(f, kind)["variation"] == key["variation"]]
    require(len(siblings) == 1, "FIELD_ADDRESS_AMBIGUOUS")
    result = {"field": key["field"]}
    for name in ("level", "variation"):
        if key[name] is not None and key[name] != 0:
            require(type(key[name]) is int and 1 <= key[name] <= 1024, "FIELD_INDEX_INVALID")
            result[name] = key[name]
    return result


def features(kind, row):
    if kind == "ability":
        return bool(row.get("level", 0) > 1 and row.get("data_index", 0) > 0 and row.get("type") in ("int", "real", "unreal"))
    if kind == "doodad":
        return bool(row.get("variation") or row.get("ordered_list"))
    if kind == "upgrade":
        return bool(row.get("level") and row.get("ordered_list"))
    return bool(row.get("ordered_list"))


def mutation(row, choices):
    old = row["value"]
    if row.get("ordered_list"):
        if len(old) > 1:
            rotated = old[1:] + old[:1]
            if rotated != old:
                return rotated
        if row.get("type") == "intList" and len(old) == 1:
            require(old[0].lstrip("-").isdigit(), "INT_LIST_NOT_DECIMAL")
            number = int(old[0])
            return [str(number + 1 if number < 2**31 - 1 else number - 1)]
        # 单项引用列表只替换为公共选项中的实际对象；不制造 rawcode 或资产路径。
        for item in choices["items"]:
            new = item.get("value")
            if isinstance(new, str) and new and new != old[0] and not any(c in new for c in ",\r\n\0\ufffd"):
                return [new]
        raise MatrixError("NO_REVERSIBLE_TYPED_MUTATION")
    require(type(old) in (int, float), "NO_REVERSIBLE_TYPED_MUTATION")
    step = 1 if row.get("type") == "int" else 0.125
    def bound(name, fallback):
        try:
            value = float(row.get(name, ""))
            return value if math.isfinite(value) else fallback
        except (TypeError, ValueError):
            return fallback
    lower, upper = bound("minimum", -math.inf), bound("maximum", math.inf)
    new = old + step if old + step <= upper else old - step
    if row.get("type") == "int":
        require(type(old) is int and -(2**31) <= new < 2**31, "NO_REVERSIBLE_TYPED_MUTATION")
        new = int(new)
    else:
        # MCP 数值经原 facade 落到 f32；合同保存其真正可回读值，不能拿 Python
        # double 的加法结果当成预期，更不能把大数加0.125后原位舍入冒称修改。
        try:
            new = struct.unpack(">f", struct.pack(">f", new))[0]
        except (OverflowError, struct.error) as error:
            raise MatrixError("NO_REVERSIBLE_TYPED_MUTATION") from error
    require(math.isfinite(new) and lower <= new <= upper and new != old, "NO_REVERSIBLE_TYPED_MUTATION")
    return new


def contract_digest(plan):
    return probe.json_sha({k: v for k, v in plan.items() if k != "contract_sha256"})


def build_plan(server, bindings, object_budget=12):
    require(1 <= object_budget <= 64, "OBJECT_SAMPLE_BUDGET")
    require(server.tool("get_revision", {}).get("transaction_active") is False, "EXISTING_TRANSACTION_REFUSED")
    plan = {"schema": SCHEMA, "implementation_sha256": probe.sha(Path(__file__)),
            "bindings": copy.deepcopy(bindings), "readonly_discovery": True,
            "saveable": "unproven: root shared Save and native replay required", "gui_verified": False,
            "native_editor_verified": False, "game_verified": False, "cases": [], "object_budget_per_kind": object_budget}
    planning_identity = None
    for kind in KINDS:
        objects = page_rows(server, "objects_list", {"kind": kind, "scope": "all"}, "items", probe.MAX_PAGES)
        identity = probe.identity(objects)
        require(planning_identity is None or identity == planning_identity, "QUERY_REVISION_CHANGED")
        planning_identity = identity
        require(len({r["id"] for r in objects["items"]}) == len(objects["items"]), "OBJECT_ID_AMBIGUOUS")
        counts = {source: sum(r.get("source") == source for r in objects["items"]) for source in ("custom", "map_override", "standard")}
        case = {"kind": kind, "member": MEMBERS[kind], "status": "blocked", "counts": counts,
                "total": objects["total"], "examined": [], "reason": "no usable complex field within bounded sample"}
        if kind == "doodad":
            case["variation_coverage"] = "unsupported: shared schema retains native variation numbering gate"
        # 每个列表明确来源；标准目录不会被标成地图 original 修改。
        ranking = {"custom": 0, "map_override": 1, "standard": 2}
        sample = sorted(objects["items"], key=lambda r: (ranking.get(r.get("source"), 3), r["id"]))[:object_budget]
        if bindings.get("skin_members", {}).get(kind):
            case["reason"] = "Skin member present: layered write not authorized"
            plan["cases"].append(case)
            continue
        for obj in sample:
            snapshot = fields(server, kind, obj["id"])
            require(snapshot["project_identity"] == objects["project_identity"] and snapshot["revision"] == objects["revision"], "QUERY_REVISION_CHANGED")
            case["examined"].append({"id": obj["id"], "source": obj["source"], "parent_id": obj.get("parent_id"),
                "field_total": snapshot["total"], "readonly_variations": [f for f in snapshot["fields"] if f.get("variation") and not f.get("editable")]})
            usable = [f for f in snapshot["fields"] if f.get("editable") and features(kind, f)]
            usable.sort(key=lambda f: (not f.get("overridden"), not (isinstance(f.get("value"), list) and len(f["value"]) > 1),
                not (f.get("level", 0) > 1), f["field"], f.get("level", 0), f.get("variation", 0)))
            for row in usable:
                try:
                    command_address = address(row, snapshot["fields"], kind)
                    raw = public_raw(row)
                    choices = options(server, kind, obj["id"], row["field"])
                    require(choices["project_identity"] == snapshot["project_identity"] and choices["revision"] == snapshot["revision"], "QUERY_REVISION_CHANGED")
                    new = mutation(row, choices)
                    new_raw = public_raw({**row, "value": new})
                    allowed = [x.get("value") for x in choices["items"]]
                    if not choices.get("free_value"):
                        require(all(x in allowed for x in (new if isinstance(new, list) else [new])), "OPTION_VALUE_UNCONFIRMED")
                except MatrixError as error:
                    # 身份或不完整查询错误必须终止，不应被降为换字段继续。
                    if str(error).startswith("QUERY_"):
                        raise
                    continue
                case.update(status="editable", reason="", object={k: obj.get(k) for k in ("id", "stable_id", "source", "parent_id", "name")},
                    full_modification_key=full_key(row, kind), command_address=command_address,
                    expected_value=copy.deepcopy(row["value"]), expected_raw=raw,
                    expected_value_source=row.get("value_source"), expected_overridden=row.get("overridden"),
                    new_typed_value=new, new_raw=new_raw, field_metadata=metadata(row), options_sha256=probe.json_sha(choices["items"]),
                    options_free_value=choices.get("free_value"),
                    planning_project_identity=snapshot["project_identity"], planning_revision=snapshot["revision"])
                break
            if case["status"] == "editable":
                break
        if kind == "doodad" and case["status"] == "blocked":
            case["reason"] = "no usable doodad ordered-list field in bounded public getter sample; variation remains unsupported"
        plan["cases"].append(case)
    plan["ready"] = all(c["status"] == "editable" for c in plan["cases"])
    plan["contract_sha256"] = contract_digest(plan)
    return plan


def validate_plan(plan):
    require(plan.get("schema") == SCHEMA and plan.get("contract_sha256") == contract_digest(plan), "PLAN_DIGEST_INVALID")
    require(plan.get("implementation_sha256") == probe.sha(Path(__file__)), "PLAN_IMPLEMENTATION_CHANGED")
    require([c.get("kind") for c in plan.get("cases", [])] == list(KINDS), "PLAN_SEVEN_KINDS_REQUIRED")
    require(plan.get("ready") is True and all(c.get("status") == "editable" for c in plan["cases"]), "PLAN_BLOCKED_NO_WRITES")
    require(len({c["planning_revision"] for c in plan["cases"]}) == 1
            and len({c["planning_project_identity"] for c in plan["cases"]}) == 1, "PLAN_MIXED_REVISION")
    for case in plan["cases"]:
        row = {**case["field_metadata"], **case["full_modification_key"], "value": case["expected_value"]}
        require(public_raw(row) == case["expected_raw"] and public_raw({**row, "value": case["new_typed_value"]}) == case["new_raw"],
                "PLAN_RAW_PROJECTION_INVALID")
        require(case["member"] == MEMBERS[case["kind"]] and case["expected_value"] != case["new_typed_value"]
                and features(case["kind"], row), "PLAN_CASE_COVERAGE_INVALID")


def find_row(reply, key, kind):
    rows = [row for row in reply["fields"] if full_key(row, kind) == key]
    require(len(rows) == 1, "FULL_MODIFICATION_KEY_NOT_UNIQUE")
    return rows[0]


def read_expected(server, case, after=False):
    reply = fields(server, case["kind"], case["object"]["id"], [case["full_modification_key"]["field"]])
    row = find_row(reply, case["full_modification_key"], case["kind"])
    require(address(row, reply["fields"], case["kind"]) == case["command_address"], "SETTER_ADDRESS_CHANGED")
    require(metadata(row) == case["field_metadata"], "FIELD_METADATA_CHANGED")
    require(row["value"] == (case["new_typed_value"] if after else case["expected_value"]), "FIELD_VALUE_CHANGED")
    require(public_raw(row) == (case["new_raw"] if after else case["expected_raw"]), "FIELD_RAW_VALUE_CHANGED")
    if not after:
        require(public_raw(row) == case["expected_raw"] and row.get("value_source") == case["expected_value_source"]
                and row.get("overridden") == case["expected_overridden"], "FIELD_RAW_PRECONDITION_CHANGED")
    return reply


def preflight(server, plan, bindings):
    validate_plan(plan)
    require(bindings == plan["bindings"], "SOURCE_OR_CATALOG_BINDING_CHANGED")
    require(server.tool("get_revision", {}).get("transaction_active") is False, "EXISTING_TRANSACTION_REFUSED")
    current = None
    for case in plan["cases"]:
        rows = page_rows(server, "objects_list", {"kind": case["kind"], "scope": "all"}, "items", probe.MAX_PAGES)
        matching = [r for r in rows["items"] if r["id"] == case["object"]["id"]]
        require(len(matching) == 1, "OBJECT_NOT_FOUND")
        require(all(matching[0].get(k) == case["object"].get(k) for k in ("stable_id", "source", "parent_id")), "OBJECT_PARENT_OR_SOURCE_CHANGED")
        reply = read_expected(server, case)
        choices = options(server, case["kind"], case["object"]["id"], case["full_modification_key"]["field"])
        require(probe.json_sha(choices["items"]) == case["options_sha256"] and choices.get("free_value") == case["options_free_value"], "FIELD_OPTIONS_CHANGED")
        identity = probe.identity(reply)
        require(rows["revision"] == reply["revision"] == choices["revision"] and rows["project_identity"] == reply["project_identity"] == choices["project_identity"], "QUERY_REVISION_CHANGED")
        # 自有进程重开会生成新的 project_identity；相同源目录的只读 revision 字符串
        # 仍应相同。未触碰目标字段的其他修改，也不能混入这个固定候选合同。
        require(reply["revision"] == case["planning_revision"], "PLANNING_REVISION_CHANGED")
        require(current is None or identity == current, "QUERY_REVISION_CHANGED")
        current = identity
    return current


def verify_plan(server, plan, after=False):
    validate_plan(plan)
    identities = []
    for case in plan["cases"]:
        objects = page_rows(server, "objects_list", {"kind": case["kind"], "scope": "all"}, "items", probe.MAX_PAGES)
        matching = [obj for obj in objects["items"] if obj["id"] == case["object"]["id"]]
        require(len(matching) == 1, "OBJECT_NOT_FOUND")
        expected_source = case["object"]["source"]
        if after and expected_source == "standard":
            expected_source = "map_override"
        require(matching[0].get("source") == expected_source
                and all(matching[0].get(key) == case["object"].get(key) for key in ("parent_id", "stable_id")),
                "OBJECT_PARENT_OR_SOURCE_CHANGED")
        reply = read_expected(server, case, after)
        require(probe.identity(objects) == probe.identity(reply), "QUERY_REVISION_CHANGED")
        identities.append(probe.identity(reply))
    require(all(x == identities[0] for x in identities), "QUERY_REVISION_CHANGED")
    return {"passed": True, "verified_cases": len(identities), "phase": "candidate-public-readback" if after else "rollback-public-readback",
            "native_wire_verified": False, "native_editor_verified": False, "game_verified": False}


def verify_bindings(plan, bindings):
    """候选目录的 inventory 会变；源图、目录定义和执行程序不允许静默变化。"""
    validate_plan(plan)
    require(all(plan["bindings"].get(key) == bindings.get(key) for key in
                ("source_sha256", "catalog", "ui_catalog_sha256", "public_object_schema_sha256", "server_sha256", "cli_sha256")),
            "VERIFY_SOURCE_OR_CATALOG_CHANGED")


def apply_plan(server, plan, bindings):
    """仅供 root runner：七项全预检，共用 commands 写；晚失败整个自有事务回滚。

    成功返回原事务 envelope，由 root 决定原 Save。这里不调用 Save/commit，不接管 GUI Undo。
    初始有事务会拒绝，防止把他人的工作当成自己的失败恢复对象。
    """
    current = preflight(server, plan, bindings)
    applied = []
    try:
        for case in plan["cases"]:
            reply = server.tool("objects_set_fields", {"kind": case["kind"], "id": case["object"]["id"],
                "changes": [{**case["command_address"], "value": case["new_typed_value"]}], **current})
            require(reply.get("transaction_active") is True and server.envelope is not None, "TRANSACTION_ENVELOPE_MISSING")
            new_identity = probe.identity(reply)
            require(new_identity["project_identity"] == current["project_identity"] and new_identity["expected_revision"] != current["expected_revision"], "EDIT_REVISION_INVALID")
            checked = read_expected(server, case, after=True)
            require(probe.identity(checked) == new_identity, "EDIT_READBACK_REVISION_CHANGED")
            current = new_identity
            applied.append({"kind": case["kind"], "id": case["object"]["id"], "full_modification_key": case["full_modification_key"]})
        verified = verify_plan(server, plan, after=True)
        return {"applied": applied, "readback": verified, "transaction": copy.deepcopy(server.envelope), "save_executed": False}
    except Exception as cause:
        try:
            server.rollback()
            verify_plan(server, plan, after=False)
        except Exception as rollback_error:
            raise MatrixError(f"EDIT_FAILED_ROLLBACK_INCOMPLETE: {cause}; {rollback_error}") from cause
        raise MatrixError(f"EDIT_FAILED_ROLLED_BACK: {cause}") from cause


def catalog_inputs(args):
    archives = []
    for name in ("war3.mpq", "war3x.mpq", "war3xlocal.mpq", "war3patch.mpq"):
        matches = [p for p in args.war3_root.iterdir() if p.name.lower() == name]
        require(len(matches) <= 1, "CATALOG_ARCHIVE_ALIAS_AMBIGUOUS")
        if matches:
            path = matches[0]
            require(path.is_file() and not path.is_symlink(), "CATALOG_ARCHIVE_NOT_REGULAR")
            archives.append({"name": name, "bytes": path.stat().st_size, "sha256": probe.sha(path)})
    require(archives, "CATALOG_ARCHIVES_MISSING")
    return {"game_archives": archives, "ydwe_ui_inventory_sha256": probe.json_sha(probe.inventory(args.ydwe_root / "ui"))}


def author_list(plan):
    lines = ["# 七类公共物编候选核对清单", "", "这是独立公共工具候选合同；不代表旧 WP2、GUI Undo、WE 保存或游戏已通过。", "",
             f"源归档 SHA：`{plan['bindings']['source_sha256']}`", f"候选合同 SHA：`{plan['contract_sha256']}`", "",
             "| 类别 | 状态 | 对象/父对象/来源 | 完整修改键 | 原值 → 新 typed 值 |", "|---|---|---|---|---|"]
    for case in plan["cases"]:
        if case["status"] != "editable":
            lines.append(f"| {case['kind']} | blocked | 已查询 {len(case['examined'])} 个真实对象 | 未开放 | {case['reason']} |")
            continue
        obj = case["object"]
        compact = lambda value: json.dumps(value, ensure_ascii=False, separators=(",", ":")).replace("|", "\\|")
        lines.append(f"| {case['kind']} | editable / save 未验 | `{obj['id']}` / `{obj['parent_id']}` / {obj['source']} | `{compact(case['full_modification_key'])}` | `{compact(case['expected_value'])}` → `{compact(case['new_typed_value'])}` |")
    lines += ["", "原名称中的 TRIGSTR 是 wire 引用；乱码投影不能作为作者名称或原 bytes 证明。", "所有候选都保留精确原值用于回滚；blocked 合同整体禁止 runner 写入。", ""]
    lines += ["装饰物 variation 的原生编号写回仍 unsupported；普通列表候选不覆盖该门禁。", ""]
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("plan", "exercise", "verify"))
    for name in ("server", "cli", "source", "workspace", "ydwe-root", "war3-root", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--plan", type=Path)
    parser.add_argument("--expected-source-sha256", required=True)
    parser.add_argument("--qt-bin", type=Path)
    parser.add_argument("--object-budget", type=int, default=12)
    for name in ("start", "request", "cli"):
        parser.add_argument("--" + name + "-timeout", type=float, default=60)
    args = parser.parse_args(argv)
    if args.mode != "plan" and args.plan is None:
        parser.error("exercise/verify require --plan")
    output = args.output.resolve()
    require(not output.is_relative_to(args.workspace.resolve()) and not args.workspace.resolve().is_relative_to(output), "OUTPUT_WORKSPACE_OVERLAP")
    output.mkdir(parents=True, exist_ok=False)
    ev = probe.Evidence(output)
    ev.receipt.update(schema=SCHEMA, evidence_level="public-object-candidate-contract", mode=args.mode, save_executed=False)
    server, source_before, workspace_before, catalog_before = None, None, None, None
    executables_before = None
    try:
        for name in ("source", "server", "cli", "workspace", "ydwe_root", "war3_root"):
            setattr(args, name, getattr(args, name).resolve(strict=True))
        source_before = probe.sha(args.source)
        require(source_before == args.expected_source_sha256 and len(source_before) == 64, "SOURCE_SHA_MISMATCH")
        workspace_before = probe.inventory(args.workspace)
        catalog_before = catalog_inputs(args)
        executables_before = {name: probe.sha(getattr(args, name)) for name in ("server", "cli")}
        env = dict(os.environ)
        env.pop("FORGE_MCP_ACCESS_TOKEN", None)
        env["PATH"] = os.pathsep.join([str(args.server.parent), str(args.cli.parent)] + ([str(args.qt_bin)] if args.qt_bin else []) + [env.get("PATH", "")])
        env["FORGE_MCP_WAR3_ROOT"], env["FORGE_MCP_YDWE_ROOT"] = str(args.war3_root), str(args.ydwe_root)
        members = probe.list_members(args, env, ev, args.source, "source")
        audit = [members[n] for n in probe.OBJECT_MEMBERS + probe.PROTECTED_MEMBERS if n in members]
        manifest = probe.extract_members(args, env, ev, args.source, audit, output / "source-members", "source-members")
        if args.mode != "verify":
            for kind, name in MEMBERS.items():
                if name in manifest:
                    require((args.workspace / probe.safe_member(name)).is_file() and probe.sha(args.workspace / probe.safe_member(name)) == manifest[name]["sha256"], "WORKSPACE_MEMBER_DIFFERS_FROM_SOURCE: " + kind)
                else:
                    require(not (args.workspace / probe.safe_member(name)).exists(), "WORKSPACE_UNEXPECTED_MEMBER: " + kind)
        bindings = {"source_sha256": source_before, "workspace_inventory_sha256": probe.json_sha(workspace_before),
            "members": {kind: manifest.get(name) for kind, name in MEMBERS.items()}, "catalog": catalog_before,
            "server_sha256": executables_before["server"], "cli_sha256": executables_before["cli"],
            "skin_members": {kind: [n for n in members if n == name.replace("war3map.", "war3mapskin.")] for kind, name in MEMBERS.items()},
            "opaque_member_coverage": "unproven by named-member enumeration; root original archive Save must retain opaque members"}
        server = probe.HttpServer(args, env, args.workspace, "object-matrix", ev).start()
        revision = server.record["initial_revision"]["revision"]
        bindings["ui_catalog_sha256"] = revision["ui_catalog_sha256"]
        bindings["workspace_source_sha256"] = revision["source_sha256"]
        listed = json.loads((output / "object-matrix.tools-list.json").read_text(encoding="utf-8"))
        bindings["public_object_schema_sha256"] = probe.json_sha([x for x in listed["tools"] if x["name"] in probe.OBJECT_TOOLS])
        if args.mode == "plan":
            plan = build_plan(server, bindings, args.object_budget)
            ev.artifact("candidate-plan.json", plan)
            (output / "AUTHOR_CHECKLIST.md").write_text(author_list(plan), encoding="utf-8")
            ev.receipt.update(passed=True, plan_ready=plan["ready"], contract_sha256=plan["contract_sha256"],
                cases=[{"kind": c["kind"], "status": c["status"], "id": c.get("object", {}).get("id"), "reason": c["reason"]} for c in plan["cases"]])
        else:
            plan = json.loads(args.plan.read_text(encoding="utf-8"))
            if args.mode == "exercise":
                ev.artifact("exercise.json", apply_plan(server, plan, bindings))
                server.rollback()
                ev.artifact("rollback.json", verify_plan(server, plan))
            else:
                verify_bindings(plan, bindings)
                ev.artifact("candidate-readback.json", verify_plan(server, plan, after=True))
            ev.receipt["passed"] = True
    except Exception as error:
        ev.receipt.update(passed=False, error=ev.scrub(str(error)))
    finally:
        if server:
            server.close()
        if source_before:
            ev.receipt["source_unchanged"] = probe.sha(args.source) == source_before
        if workspace_before is not None:
            ev.receipt["workspace_unchanged"] = probe.inventory(args.workspace) == workspace_before
        if catalog_before is not None:
            ev.receipt["catalog_unchanged"] = catalog_inputs(args) == catalog_before
        if executables_before is not None:
            ev.receipt["executables_unchanged"] = all(probe.sha(getattr(args, name)) == sha for name, sha in executables_before.items())
        for name in ("source_unchanged", "workspace_unchanged", "catalog_unchanged", "executables_unchanged"):
            if ev.receipt.get(name) is False:
                ev.receipt["passed"] = False
        if ev.receipt["cleanup_errors"]:
            ev.receipt["passed"] = False
        ev.checkpoint("closed")
        ev.log.close()
    print(json.dumps({"passed": ev.receipt["passed"], "plan_ready": ev.receipt.get("plan_ready"),
        "receipt": str(output / "receipt.json"), "save_executed": False}, ensure_ascii=False))
    return 0 if ev.receipt["passed"] and ev.receipt.get("plan_ready", True) else 4


if __name__ == "__main__":
    try:
        sys.exit(main())
    except MatrixError as error:
        print(str(error), file=sys.stderr)
        sys.exit(4)
