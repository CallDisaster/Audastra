"""独立 MCP HTTP 变量组：原事务保存、新服务回读与 WTG 字节边界验收。

复用同目录物编探针的 HTTP 协商、凭据脱敏、自有进程清理及现有 CLI 提取。
本探针要求真实地图具有未审计的不透明代码，变量改名与删除必须明确拒绝。
新增 integer 标量的初值是字面值文本，绝不提交作者 JASS。仅写新证据目录，
不回写源归档或输入目录；不等价于 GUI 助手、原生编辑器或游戏运行验收。
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import struct

from mcp_object_http_probe import (
    Evidence, HttpServer, MAX_PAGES, OBJECT_MEMBERS, extract_members, inventory,
    json_sha, list_members, normalized_member, require, safe_member, sha,
)


VARIABLE_TOOLS = {
    "variables_list", "variables_get", "variables_create", "variables_update",
    "variables_rename", "variables_delete",
}
UNCHANGED_MEMBERS = ("war3map.wct", "tiara.ai", "tiara.blp") + OBJECT_MEMBERS
MAX_WTG_BYTES = 64 * 1024 * 1024
MAX_DECLARATIONS = 65536
MAX_TEXT_BYTES = 1024 * 1024
INITIAL_VALUE, UPDATED_VALUE = "17", "29"


class VariableHttpServer(HttpServer):
    def start(self):
        # 不复制原 HTTP 实现；组 B 的服务仍须保留已验收的组 A 与原事务工具。
        super().start()
        listed = json.loads((self.evidence.output / f"{self.label}.tools-list.json").read_text(encoding="utf-8"))
        definitions = {tool.get("name"): tool for tool in listed.get("tools", [])}
        required = VARIABLE_TOOLS | {"begin_edit"}
        require(required <= definitions.keys(),
                f"{self.label} variable/transaction tools missing: {sorted(required - definitions.keys())}")
        for name in VARIABLE_TOOLS:
            definition = definitions[name]
            schema = definition.get("inputSchema")
            require(isinstance(schema, dict) and schema.get("type") == "object",
                    f"{name} has no actual object input schema")
            required = set(schema.get("required", []))
            if name in {"variables_create", "variables_update", "variables_rename", "variables_delete"}:
                require({"project_identity", "expected_revision"} <= required,
                        f"{name} actual schema does not require the opaque revision guard")
                require(schema.get("properties", {}).get("expected_revision", {}).get("type") == "string",
                        f"{name} expected_revision must be an opaque string")
        self.evidence.check(f"{self.label}-actual-six-variable-tool-schemas", {
            "names": sorted(VARIABLE_TOOLS),
            "schema_sha256": json_sha({name: definitions[name] for name in sorted(VARIABLE_TOOLS)}),
        })
        return self


def context(reply):
    require(isinstance(reply.get("project_identity"), str) and reply["project_identity"],
            "variable reply has no actual project identity")
    require(isinstance(reply.get("revision"), str) and reply["revision"],
            "variable reply has no opaque revision token")
    return {"project_identity": reply["project_identity"], "expected_revision": reply["revision"]}


def variable_id(row):
    reference = row.get("ref", {})
    value = reference.get("id")
    require(reference.get("kind") == "variable" and isinstance(value, str) and
            re.fullmatch(r"[1-9][0-9]{0,19}", value) and int(value) <= 2**64 - 1,
            "variable row has no canonical nonzero 64-bit StableId string")
    return value


def variable_semantic(row):
    variable_id(row)
    result = {}
    for key in ("name", "type_name", "value"):
        value = row.get(key)
        require(isinstance(value, dict) and isinstance(value.get("text"), str),
                f"variable row lacks actual RawText {key}")
        result[key] = value["text"]
    flags = row.get("flags")
    require(isinstance(flags, dict) and all(key in flags for key in
            ("unknown", "is_array", "array_size", "initialized")), "variable row flags are incomplete")
    result["flags"] = flags
    return result


def list_variables(server):
    rows, first = [], None
    for _ in range(MAX_PAGES):
        reply = server.tool("variables_list", {"page": {"offset": len(rows), "limit": 128}})
        context(reply)
        page = reply.get("rows")
        require(isinstance(page, list) and len(page) <= 128, "variable list lacks a bounded actual row page")
        total = reply.get("total")
        require((isinstance(total, int) and not isinstance(total, bool)) or
                (isinstance(total, str) and re.fullmatch(r"0|[1-9][0-9]*", total)), "invalid variable list total")
        total = int(total)
        require(total <= MAX_DECLARATIONS and reply.get("start") == len(rows), "variable pagination bounds changed")
        if first is None:
            first = reply
        else:
            require(context(reply) == context(first) and reply.get("binding") == first.get("binding") and
                    int(first["total"]) == total, "variable identity or binding changed during pagination")
        for row in page:
            variable_semantic(row)
        rows.extend(page)
        require(len(rows) <= total, "variable list exceeded its actual total")
        require(reply.get("truncated") is (len(rows) < total), "variable truncation flag disagrees with actual page")
        if len(rows) == total:
            require(len({variable_id(row) for row in rows}) == len(rows), "ambiguous variable StableIds")
            require(len({row["name"]["text"] for row in rows}) == len(rows), "ambiguous variable names")
            return rows, reply
        require(page, "variable pagination stalled")
    raise RuntimeError("variable pagination exceeded the probe page budget")


def get_variable(server, identity):
    reply = server.tool("variables_get", {"id": identity})
    context(reply)
    require(reply.get("id") == identity and isinstance(reply.get("variable"), dict) and
            variable_id(reply["variable"]) == identity, "variable get returned another StableId")
    variable_semantic(reply["variable"])
    return reply


def assert_integer(row, name, value):
    actual = variable_semantic(row)
    require(actual["name"] == name and actual["type_name"] == "integer" and actual["value"] == value,
            "actual integer declaration name/type/literal does not match the authorized edit")
    flags = actual["flags"]
    require(flags["is_array"] in (False, 0) and flags["array_size"] == 1 and
            flags["initialized"] in (True, 1), "actual declaration is not an initialized integer scalar")
    return actual


def refusal_without_publication(server, evidence, name, arguments, before, expected_code):
    refused = server.rejection(name, arguments, expected_code)
    current = get_variable(server, before["id"])
    require(current["variable"] == before["variable"] and context(current) == context(before) and
            current.get("binding") == before.get("binding"), f"{name} refusal changed the current variable or revision")
    evidence.check(f"{name}-{expected_code}-atomic-refusal", {
        "refusal": refused, "id": before["id"], "current_variable_preserved": True,
        "revision_preserved": True, "binding_preserved": True,
    })
    return current


def read_wtg_variable_prefix(path):
    """只读经典 v7 声明前缀；其余触发器正文保留完整字节用于独立比较。

    布局来自实际 WtgDecoder / WtgWctCodec serializer。不是通用 WTG 编辑器，
    不解析或执行 ECA、9527、作者代码；扩展版本明确拒绝。
    """
    require(path.stat().st_size <= MAX_WTG_BYTES, "WTG exceeds 64 MiB probe budget")
    data = path.read_bytes()
    offset = 0

    def i32():
        nonlocal offset
        require(offset + 4 <= len(data), "truncated WTG declaration prefix")
        result = struct.unpack_from("<i", data, offset)[0]
        offset += 4
        return result

    def string():
        nonlocal offset
        end = data.find(b"\0", offset, min(len(data), offset + MAX_TEXT_BYTES + 1))
        require(end >= offset, "WTG declaration string is truncated or over budget")
        result = data[offset:end]
        offset = end + 1
        return result

    def count(label):
        result = i32()
        require(0 <= result <= MAX_DECLARATIONS, f"WTG {label} count outside probe budget")
        return result

    require(data[:4] == b"WTG!", "actual member is not WTG")
    offset = 4
    require(i32() == 7, "independent declaration-byte proof supports classic WTG v7 only")
    for _ in range(count("category")):
        i32()
        string()
        i32()
    require(i32() == 2, "WTG v7 variable header must be 2")
    count_offset = offset
    total = count("variable")
    rows = []
    for _ in range(total):
        start = offset
        name, type_name = string(), string()
        unknown, is_array, array_size, initialized = i32(), i32(), i32(), i32()
        value = string()
        rows.append({"name": name, "type_name": type_name, "unknown": unknown,
                     "is_array": is_array, "array_size": array_size,
                     "initialized": initialized, "value": value, "bytes": data[start:offset]})
    require(offset + 4 <= len(data), "WTG trigger suffix is missing")
    return {"sha256": sha(path), "prefix": data[:count_offset], "rows": rows,
            "suffix": data[offset:], "total": total}


def verify_wtg_delta(source, candidate, name, original_rows):
    require(source["prefix"] == candidate["prefix"], "candidate changed WTG categories/version/header")
    require(candidate["total"] == source["total"] + 1 and source["total"] == len(original_rows),
            "candidate did not append exactly one actual WTG variable declaration")
    require([row["bytes"] for row in candidate["rows"][:-1]] == [row["bytes"] for row in source["rows"]],
            "candidate changed/reordered an original WTG variable record")
    require(source["suffix"] == candidate["suffix"], "candidate changed the original trigger/ECA/argument byte suffix")
    appended = candidate["rows"][-1]
    require(appended["name"] == name.encode("ascii") and appended["type_name"] == b"integer" and
            appended["is_array"] == 0 and appended["array_size"] == 1 and appended["initialized"] == 1 and
            appended["value"] == UPDATED_VALUE.encode("ascii"), "actual appended WTG record does not preserve the edited scalar literal")
    require(source["sha256"] != candidate["sha256"], "actual WTG member did not change")
    return {
        "source_wtg_sha256": source["sha256"], "candidate_wtg_sha256": candidate["sha256"],
        "source_variable_count": source["total"], "candidate_variable_count": candidate["total"],
        "new_variable_name": name, "type_name": "integer", "initial_value": UPDATED_VALUE,
        "actual_new_record_sha256": hashlib.sha256(appended["bytes"]).hexdigest(),
        "original_variable_records_byte_exact": True, "category_header_byte_exact": True,
        "trigger_eca_argument_suffix_byte_exact": True,
        "trigger_suffix_sha256": hashlib.sha256(source["suffix"]).hexdigest(),
        "full_wtg_parser_or_runtime_proof": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("server", "cli", "source", "workspace", "ydwe-root", "war3-root", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--qt-bin", type=Path, help="仅向自有子进程 PATH 添加 Qt 运行库目录")
    parser.add_argument("--expected-source-sha256", required=True, help="授权源归档的64位小写SHA-256")
    parser.add_argument("--variable-name", help="可选的未使用ASCII变量名；默认生成随机唯一名字")
    parser.add_argument("--start-timeout", type=float, default=60)
    parser.add_argument("--request-timeout", type=float, default=60)
    parser.add_argument("--save-timeout", type=float, default=900)
    parser.add_argument("--cli-timeout", type=float, default=60)
    args = parser.parse_args()
    require(re.fullmatch(r"[0-9a-f]{64}", args.expected_source_sha256), "expected source SHA-256 must be 64 lowercase hex digits")
    for key in ("start_timeout", "request_timeout", "save_timeout", "cli_timeout"):
        require(0 < getattr(args, key) <= 3600, "timeouts must be positive and no greater than one hour")
    if args.variable_name:
        require(re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,95}", args.variable_name), "probe variable name must be an ASCII identifier")
    args.output = args.output.resolve()
    require(not args.output.is_relative_to(args.workspace.resolve()), "evidence output must be outside the input workspace")
    args.output.mkdir(parents=True, exist_ok=False)
    evidence = Evidence(args.output)
    evidence.receipt["schema"] = "audastra-mcp-variables-http-probe-v1"
    evidence.receipt["strict_opaque_rename_delete_refusals_required"] = True
    servers, source_before, workspace_before = [], None, None
    try:
        for name in ("server", "cli", "source", "workspace", "ydwe_root", "war3_root", "qt_bin"):
            value = getattr(args, name)
            if value is not None:
                setattr(args, name, value.resolve(strict=True))
        require(args.server.is_file() and args.cli.is_file() and args.source.is_file(), "binary/source path is not a file")
        require(args.source.suffix.lower() in (".w3x", ".w3m"), "source must be a map archive")
        require(args.workspace.is_dir() and (args.ydwe_root / "ui").is_dir() and args.war3_root.is_dir(),
                "workspace or trusted root/ui directory is missing")
        source_before = sha(args.source)
        require(source_before == args.expected_source_sha256, "authorized source archive SHA-256 mismatch")
        workspace_before = inventory(args.workspace)
        evidence.artifact("source-workspace.before.json", workspace_before)
        evidence.receipt.update({"source": str(args.source), "workspace": str(args.workspace),
            "source_sha256_before": source_before, "expected_source_sha256": args.expected_source_sha256,
            "server": str(args.server), "server_sha256": sha(args.server),
            "cli": str(args.cli), "cli_sha256": sha(args.cli), "ydwe_root": str(args.ydwe_root),
            "ui_data": str(args.ydwe_root / "ui"), "war3_root": str(args.war3_root),
            "workspace_inventory_sha256_before": json_sha(workspace_before)})
        evidence.checkpoint("input-identities-captured")
        env = dict(os.environ)
        env.pop("FORGE_MCP_ACCESS_TOKEN", None)
        path_entries = [str(args.server.parent), str(args.cli.parent)]
        if args.qt_bin:
            require(args.qt_bin.is_dir(), "Qt runtime directory does not exist")
            path_entries.insert(0, str(args.qt_bin))
        env["PATH"] = os.pathsep.join(path_entries + [env.get("PATH", "")])
        env["FORGE_MCP_YDWE_ROOT"] = str(args.ydwe_root)
        env["FORGE_MCP_WAR3_ROOT"] = str(args.war3_root)

        source_members = list_members(args, env, evidence, args.source, "source")
        require(all(name in source_members for name in ("war3map.wtg", "war3map.wct")), "actual archive lacks the required WTG/WCT pair")
        audit_names = [source_members[name] for name in ("war3map.wtg",) + UNCHANGED_MEMBERS if name in source_members]
        source_audit_dir = args.output / "source-member-hashes"
        source_audit = extract_members(args, env, evidence, args.source, audit_names, source_audit_dir, "source-audit")
        # 归档与目录必须是同一对触发器来源；不能把另一个目录当此归档的保存输入。
        for member in ("war3map.wtg", "war3map.wct"):
            workspace_member = args.workspace / safe_member(source_members[member])
            require(workspace_member.is_file() and sha(workspace_member) == source_audit[member]["sha256"],
                    f"input workspace {member} differs from the authorized source archive")
        source_wtg = read_wtg_variable_prefix(source_audit_dir / safe_member(source_members["war3map.wtg"]))
        source_server = VariableHttpServer(args, env, args.workspace, "source", evidence)
        servers.append(source_server)
        source_server.start()
        started = source_server.tool("begin_edit", {
            "expected_revision": source_server.record["initial_revision"]["revision"]})
        require(isinstance(started.get("transaction_id"), str) and isinstance(started.get("base_revision"), dict),
                "original begin_edit did not return the actual transaction/base revision")
        source_server.envelope = {"transaction_id": started["transaction_id"], "expected_revision": started["base_revision"]}
        original_rows, listed = list_variables(source_server)
        require(source_wtg["total"] == len(original_rows), "source HTTP variables disagree with actual WTG declaration count")
        evidence.artifact("source-variables.before.json", {"rows": original_rows, "binding": listed.get("binding"), **context(listed)})
        name = args.variable_name or "AudastraHttpVariable_" + secrets.token_hex(8)
        require(name not in {row["name"]["text"] for row in original_rows}, "probe variable name already exists")
        created = source_server.tool("variables_create", {**context(listed), "name": name,
            "type_name": "integer", "is_array": False, "array_size": 1,
            "initialized": True, "initial_value": INITIAL_VALUE})
        require(created.get("before") is None and created.get("changed") is True and created.get("saved") is False,
                "variable create did not return an unsaved original transaction change")
        identity = created.get("id")
        before = get_variable(source_server, identity)
        assert_integer(before["variable"], name, INITIAL_VALUE)
        require(created.get("after") == before["variable"] and context(created) == context(before),
                "create receipt differs from the actual current variable readback")
        rows_created, _ = list_variables(source_server)
        require([variable_semantic(row) for row in rows_created] ==
                [variable_semantic(row) for row in original_rows] + [variable_semantic(before["variable"])],
                "creation changed/reordered an original variable or created additional declarations")
        evidence.check("actual-new-integer-scalar-created-in-original-transaction", {"id": identity, "name": name, "receipt": created})

        changed = source_server.tool("variables_update", {"id": identity, **context(before), "changes": {"initial_value": UPDATED_VALUE}})
        after = get_variable(source_server, identity)
        assert_integer(after["variable"], name, UPDATED_VALUE)
        require(changed.get("before") == before["variable"] and changed.get("after") == after["variable"] and
                changed.get("changed") is True and changed.get("saved") is False and context(after) != context(before),
                "variable literal update receipt or actual revision did not advance correctly")
        evidence.artifact("created-variable.updated.json", after)
        evidence.receipt["selected"] = {"source_id": identity, "name": name, "type_name": "integer",
                                        "initial_literal": INITIAL_VALUE, "updated_literal": UPDATED_VALUE}
        evidence.check("actual-variable-literal-update", changed)
        after = refusal_without_publication(source_server, evidence, "variables_update",
            {"id": identity, **context(before), "changes": {"initial_value": INITIAL_VALUE}}, after, "VARIABLE_REVISION_CONFLICT")
        # 标量长度2必须由原领域规则拒绝，而不是由探针或外壳修正。
        after = refusal_without_publication(source_server, evidence, "variables_update",
            {"id": identity, **context(after), "changes": {"initialized": False, "array_size": 2}}, after, "TRIGGER_GRAPH_VARIABLE_ARRAY")
        after = refusal_without_publication(source_server, evidence, "variables_update",
            {"id": identity, **context(after), "changes": {"initial_value": "1 + 2"}}, after, "TRIGGER_LITERAL_INVALID")
        # facade 以 type_name→initial_value 顺序形成 GraphSet：有效非 no-op 前缀也不能部分发布。
        after = refusal_without_publication(source_server, evidence, "variables_update",
            {"id": identity, **context(after), "changes": {"type_name": "real", "initial_value": "not-a-real"}}, after, "TRIGGER_LITERAL_INVALID")
        after = refusal_without_publication(source_server, evidence, "variables_rename",
            {"id": identity, **context(after), "name": name + "_Renamed"}, after, "TRIGGER_RENAME_CODE_AUDIT_REQUIRED")
        after = refusal_without_publication(source_server, evidence, "variables_delete",
            {"id": identity, **context(after)}, after, "TRIGGER_GRAPH_REMOVE_REFERENCE_UNKNOWN")
        require(source_server.envelope is not None, "variable writes did not expose the original active transaction envelope")
        preview = source_server.tool("preview_edit", source_server.envelope)
        evidence.artifact("transaction.preview.json", preview)
        changes = preview.get("changes")
        require(isinstance(changes, list) and len(changes) == 2,
                "variable preview must contain exactly the original paired WTG/WCT trigger patches")
        trigger_changes = {}
        for change in changes:
            require(isinstance(change, dict) and change.get("kind") == "trigger" and isinstance(change.get("path"), str),
                    "variable preview contains a non-trigger or invalid member entry")
            member = normalized_member(change["path"])
            require(member in ("war3map.wtg", "war3map.wct") and member not in trigger_changes,
                    "variable preview contains an extra member or duplicate paired member")
            trigger_changes[member] = change
        require(set(trigger_changes) == {"war3map.wtg", "war3map.wct"}, "variable preview lost one paired trigger member")
        binding = after.get("binding")
        require(isinstance(binding, dict), "current variable reply has no actual trigger binding")
        wtg_change, wct_change = trigger_changes["war3map.wtg"], trigger_changes["war3map.wct"]
        require(wtg_change.get("before_sha256") == source_audit["war3map.wtg"]["sha256"] and
                wtg_change.get("after_sha256") == binding.get("current_wtg_sha256") and
                isinstance(wtg_change.get("after_sha256"), str) and re.fullmatch(r"[0-9a-f]{64}", wtg_change["after_sha256"]) and
                wtg_change["before_sha256"] != wtg_change["after_sha256"],
                "WTG preview is not bound to the actual extracted source and current changed variable overlay")
        source_wct_sha = source_audit["war3map.wct"]["sha256"]
        require(wct_change.get("before_sha256") == wct_change.get("after_sha256") == source_wct_sha and
                binding.get("current_wct_sha256") == source_wct_sha,
                "WCT companion preview did not preserve the independently extracted original WCT bytes")
        evidence.check("paired-preview-bound-to-source-and-active-variable-overlay", {
            "exact_unique_members": sorted(trigger_changes), "wtg_changed": wtg_change,
            "wct_unchanged_source_companion": wct_change, "current_binding": binding,
        })
        validated = source_server.tool("validate_edit", source_server.envelope)
        require(validated.get("satisfied") is True and validated.get("highest_proven_level", -1) >= 0,
                "latest variable transaction did not validate")
        evidence.check("existing-transaction-preview-and-validation", {"preview": preview, "validation": validated, "envelope": source_server.envelope})
        candidate = args.output / ("mcp-variables-candidate" + args.source.suffix.lower())
        require(not candidate.exists(), "candidate already exists")
        saved = source_server.tool("save_map_edit_candidate", {"source_map_path": str(args.source),
            "output_path": str(candidate), "expected_source_sha256": source_before,
            **source_server.envelope, "script_target": "source"}, timeout=args.save_timeout)
        require(candidate.is_file() and saved.get("candidate_kind") == "mpq-map-archive" and
                saved.get("source_sha256") == source_before and saved.get("candidate_sha256") == sha(candidate) and
                saved.get("script_rebuilt") is True and saved.get("native_editor_verified") is False and
                saved.get("game_runtime_verified") is False, "save receipt identities or acceptance claims are invalid")
        require(isinstance(saved.get("output_path"), str) and Path(saved["output_path"]).resolve(strict=True) == candidate,
                "save receipt does not identify the actual output candidate")
        trigger_receipt = saved.get("trigger_transaction_receipt")
        require(isinstance(trigger_receipt, dict) and trigger_receipt.get("transaction_id") == source_server.envelope["transaction_id"] and
                trigger_receipt.get("variable_count") == len(original_rows) + 1 and
                trigger_receipt.get("transaction_active") is True and trigger_receipt.get("workspace_rebound") is False,
                "save did not consume the same complete variable/trigger transaction")
        evidence.receipt["saved"], evidence.receipt["candidate"] = saved, str(candidate)
        evidence.check("original-save-service-produced-real-variable-candidate", saved)
        source_server.rollback()
        rolled_back, _ = list_variables(source_server)
        require(rolled_back == original_rows, "transaction rollback did not restore the exact original variable list")
        source_server.rejection("variables_get", {"id": identity}, "MCP_TRIGGER_GRAPH_VARIABLE_MISSING")
        evidence.check("source-variable-overlay-rolled-back-after-save", {"created_id_absent": identity, "original_rows_exact": True})
        source_server.close()

        candidate_members = list_members(args, env, evidence, candidate, "candidate")
        candidate_workspace = args.output / "candidate-workspace"
        candidate_manifest = extract_members(args, env, evidence, candidate, list(candidate_members.values()), candidate_workspace, "candidate")
        require("war3map.wtg" in candidate_manifest and "war3map.wct" in candidate_manifest, "candidate lost the synchronized trigger pair")
        wtg_receipts = [row for row in saved.get("edited_members", []) if normalized_member(row.get("name", "")) == "war3map.wtg"]
        require(len(wtg_receipts) == 1 and wtg_receipts[0].get("original_sha256") == source_audit["war3map.wtg"]["sha256"] and
                wtg_receipts[0].get("saved_sha256") == candidate_manifest["war3map.wtg"]["sha256"],
                "WTG save receipt is not bound to actual original and extracted candidate members")
        protected = {}
        for member in UNCHANGED_MEMBERS:
            original, copied = source_audit.get(member), candidate_manifest.get(member)
            require((original is None) == (copied is None) and (original is None or original["sha256"] == copied["sha256"]),
                    f"unchanged protected member changed: {member}")
            protected[member] = {"present": original is not None, "source_sha256": original["sha256"] if original else None,
                                 "candidate_sha256": copied["sha256"] if copied else None, "body_inspected": False}
        evidence.check("wct-code-pair-and-all-native-object-members-preserved", protected)
        candidate_wtg = read_wtg_variable_prefix(candidate_workspace / safe_member(candidate_members["war3map.wtg"]))
        proof = verify_wtg_delta(source_wtg, candidate_wtg, name, original_rows)
        evidence.artifact("candidate-variable.actual-wtg-byte-proof.json", proof)
        evidence.check("actual-wtg-one-variable-appended-original-records-and-trigger-suffix-preserved", proof)
        candidate_server = VariableHttpServer(args, env, candidate_workspace, "candidate", evidence)
        servers.append(candidate_server)
        candidate_server.start()
        reopened_rows, reopened_list = list_variables(candidate_server)
        require([variable_semantic(row) for row in reopened_rows] ==
                [variable_semantic(row) for row in original_rows] + [variable_semantic(after["variable"])],
                "second HTTP service changed/lost an original variable or did not read the saved updated declaration")
        actual_new = [row for row in reopened_rows if row["name"]["text"] == name]
        require(len(actual_new) == 1, "second HTTP service did not find exactly one saved variable name")
        reopened = get_variable(candidate_server, variable_id(actual_new[0]))
        assert_integer(reopened["variable"], name, UPDATED_VALUE)
        require(reopened["variable"] == actual_new[0] and reopened.get("binding") == reopened_list.get("binding"),
                "second HTTP get does not match its actual saved list/binding")
        require(candidate_server.record["initial_revision"]["revision"]["ui_catalog_sha256"] ==
                source_server.record["initial_revision"]["revision"]["ui_catalog_sha256"], "candidate reopened with another catalog")
        evidence.artifact("candidate-variable.http-readback.json", reopened)
        evidence.check("new-http-server-exact-saved-variable-readback", {
            "name": name, "source_transaction_id": identity, "reopened_stable_id": reopened["id"],
            "initial_value": UPDATED_VALUE, "original_variable_semantics_and_order_preserved": True,
            "stable_id_across_archive_reparse_claimed": False, "candidate_sha256": sha(candidate),
            "wtg_sha256": candidate_manifest["war3map.wtg"]["sha256"], "wct_sha256": candidate_manifest["war3map.wct"]["sha256"],
        })
        candidate_server.close()
        require(sha(args.source) == source_before, "source archive changed during HTTP probe")
        require(inventory(args.workspace) == workspace_before, "input expanded workspace changed during HTTP probe")
        require(not evidence.receipt["cleanup_errors"], "owned server cleanup failed")
        evidence.receipt["passed"] = True
        evidence.checkpoint("passed-independent-variables-http-and-archive-readback")
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
                evidence.receipt["source_sha256_after"] = source_after
                evidence.receipt["source_unchanged"] = source_after == source_before
            if workspace_before is not None:
                after_inventory = inventory(args.workspace)
                evidence.artifact("source-workspace.after.json", after_inventory)
                evidence.receipt["workspace_inventory_sha256_after"] = json_sha(after_inventory)
                evidence.receipt["source_workspace_unchanged"] = after_inventory == workspace_before
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
