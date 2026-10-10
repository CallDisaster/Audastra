"""复用项目 CLI 逐 MPQ 成员比较；未命名块与未解释差异明确阻塞验收。"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys
import tempfile

sys.dont_write_bytecode = True

_spec = importlib.util.spec_from_file_location("p1_support", Path(__file__).with_name("p1-support.py"))
support = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(support)

RESERVED = {"(listfile)", "(attributes)", "(signature)"}


def canonical(name):
    support.require(isinstance(name, str) and name and "\n" not in name and "\r" not in name and "\x00" not in name,
                    "成员名字为空或不能由现有 CLI 文本清单表示")
    return name.replace("/", "\\").casefold()


def cli_run(cli, *args, timeout=120):
    return support.run_command([str(cli), *map(str, args)], timeout=timeout)


def archive_inventory(cli, archive):
    before = support.sha256(archive)
    legacy = cli_run(cli, "archive-session-inspect", archive)
    fields = dict(line.split("=", 1) for line in legacy.splitlines() if "=" in line)
    support.require(fields.get("sourceSha256", "").lower() == before, "CLI 来源SHA与文件不同")
    support.require(fields.get("opaqueMemberCount", "").isdigit(), "CLI 没有不透明成员数量")
    audit = cli_run(cli, "mpq-hash-inspect", archive)
    block = re.search(r"(?m)^block entries=\d+ existing=(\d+) unreferenced=(\d+) aliased=(\d+) anonymous=(\d+)\s*$", audit)
    support.require(block is not None, "现有 CLI 的块审计格式不符")
    existing, unreferenced, aliased, anonymous = map(int, block.groups())
    support.require(not unreferenced and not aliased, "未引用或别名块不能逐成员证明")
    support.require("  unresolved:" not in audit, "listfile 含无法解析成员")
    listed = cli_run(cli, "mpq-list", archive).splitlines()
    support.require(listed and re.fullmatch(r"total=\d+ shown=\d+", listed[-1]), "CLI 清单结尾无数量")
    count = int(listed[-1].split()[0].split("=")[1])
    support.require(count == len(listed) - 1 and listed[-1] == f"total={count} shown={count}", "CLI 清单数量不符")
    names = {}
    for name in listed[:-1]:
        key = canonical(name)
        support.require(key not in names, "成员归一化后重名，不能明确比较")
        names[key] = name
    for name in re.findall(r"(?m)^  special: (.+)$", audit):
        names.setdefault(canonical(name), name)
    support.require(len(names) + anonymous == existing, "命名/匿名块数量未完整对上审计")
    support.require(support.sha256(archive) == before, "归档在盘点期间变化")
    return {"source_sha256": before, "names": names, "existing_blocks": existing,
            "anonymous_blocks": anonymous, "opaque_members": int(fields["opaqueMemberCount"])}


def member_identities(cli, archive, inventory, keys):
    identities = {}
    payload = [key for key in keys if key not in RESERVED]
    # 现有精确查询一次最多256个；32个也避免Windows命令行总长度预算。
    for offset in range(0, len(payload), 32):
        batch = payload[offset:offset + 32]
        args = [str(cli), "archive-session-inspect", str(archive), "--expected-source-sha256", inventory["source_sha256"]]
        for key in batch:
            args += ["--member", inventory["names"].get(key, key)]
        reply = support.command_json(args)
        support.require(reply.get("source_sha256") == inventory["source_sha256"], "精确身份来源SHA不符")
        rows = reply.get("members")
        support.require(isinstance(rows, list) and len(rows) == len(batch), "精确身份回读数量不符")
        for requested, row in zip(batch, rows):
            support.require(canonical(row.get("name")) == requested, "精确身份回读成员顺序/名字不符")
            state = row.get("state")
            support.require(state in ("present", "missing"), "精确身份没有明确存在状态")
            if state == "present":
                support.require(isinstance(row.get("sha256"), str) and re.fullmatch(r"[0-9a-f]{64}", row["sha256"]), "成员SHA无效")
                support.require(str(row.get("bytes", "")).isdigit(), "成员字节数无效")
            else:
                support.require(row.get("sha256") is None and row.get("bytes") is None, "不存在成员却有身份")
            support.require((requested in inventory["names"]) == (state == "present"), "精确存在状态与完整清单不同")
            identities[requested] = {"state": state, "bytes": int(row["bytes"]) if state == "present" else None,
                                     "sha256": row["sha256"]}
    # 特殊成员由项目现有提取器读取，不重新实现 MPQ 解码器。
    with tempfile.TemporaryDirectory(prefix="audastra-p1-metadata-") as temp:
        for index, key in enumerate(key for key in keys if key in RESERVED):
            if key not in inventory["names"]:
                identities[key] = {"state": "missing", "bytes": None, "sha256": None}
                continue
            path = Path(temp) / str(index)
            extracted = cli_run(cli, "mpq-extract", archive, inventory["names"][key], path)
            support.require(path.is_file(), "现有 CLI 未写出特殊成员")
            receipt = re.fullmatch(r"extracted=(.+) bytes=(\d+)\s*", extracted)
            support.require(receipt is not None and canonical(receipt[1]) == key and
                            int(receipt[2]) == path.stat().st_size, "特殊成员提取数量/身份回执不符")
            identities[key] = {"state": "present", "bytes": path.stat().st_size, "sha256": support.sha256(path)}
    support.require(support.sha256(archive) == inventory["source_sha256"], "归档在成员比较期间变化")
    return identities


def explain_differences(differences, explanations):
    support.require(isinstance(explanations, list), "解释必须是 JSON 数组")
    by_name = {}
    for explanation in explanations:
        support.require(isinstance(explanation, dict), "解释条目必须为对象")
        key = canonical(explanation.get("member"))
        support.require(key not in by_name, "解释重名")
        by_name[key] = explanation
    for row in differences:
        note = by_name.pop(canonical(row["member"]), None)
        row["explanation_status"] = "unexplained"
        if note:
            support.require(note.get("before_sha256") == row["before"]["sha256"] and
                            note.get("after_sha256") == row["after"]["sha256"], "解释必须绑定本次前后SHA")
            support.require(isinstance(note.get("reason"), str) and note["reason"].strip() and
                            isinstance(note.get("evidence"), str) and note["evidence"].strip(), "每个解释都须有具体原因及证据引用")
            row.update(explanation_status="explained_pending_review", reason=note["reason"], evidence=note["evidence"])
    support.require(not by_name, "解释包含本次不存在的差异，可能用了旧回执")
    return differences


def compare(cli, before, after, explanations=None):
    cli, before, after = map(support.plain_path, (cli, before, after))
    support.require(before != after, "必须比较两个独立候选")
    left, right = archive_inventory(cli, before), archive_inventory(cli, after)
    coverage = not any(item["anonymous_blocks"] or item["opaque_members"] for item in (left, right))
    keys = sorted(left["names"].keys() | right["names"].keys())
    lhs, rhs = member_identities(cli, before, left, keys), member_identities(cli, after, right, keys)
    differences = [{"member": left["names"].get(key, right["names"].get(key)), "before": lhs[key], "after": rhs[key]}
                   for key in keys if lhs[key] != rhs[key]]
    explain_differences(differences, explanations or [])
    # 即使所有命名差异都有说明，匿名块也不提升为全归档已证明。
    complete = coverage and all(row["explanation_status"] == "explained_pending_review" for row in differences)
    return {"schema": "p1-mpq-member-comparison-v1", "status": "reviewable" if complete else "blocked",
            "before_sha256": left["source_sha256"], "after_sha256": right["source_sha256"],
            "named_members_compared": len(keys), "unchanged_named_members": len(keys) - len(differences),
            "coverage_complete": coverage, "anonymous_blocks": [left["anonymous_blocks"], right["anonymous_blocks"]],
            "opaque_members": [left["opaque_members"], right["opaque_members"]], "differences": differences,
            "source_unchanged": True, "original_editor_validated": False, "runtime_validated": False,
            "limitations": ["比较的是解码成员身份；压缩布局、归档前缀及原版窗口语义需另行审查",
                            "reviewable 仅表示差异说明齐备，说明内容仍需独立审查"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--before", required=True, type=Path)
    parser.add_argument("--after", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--explanations", type=Path)
    args = parser.parse_args()
    support.require(not args.report.exists(), "报告路径已存在")
    try:
        result = compare(args.cli, args.before, args.after, support.read_json(args.explanations) if args.explanations else [])
    except Exception as error:
        support.write_json(args.report, {"schema": "p1-mpq-member-comparison-v1", "status": "failed",
                                        "coverage_complete": False, "reason": str(error), "runtime_validated": False})
        raise
    support.write_json(args.report, result)
    print(json.dumps({"status": result["status"], "differences": len(result["differences"]),
                      "coverage_complete": result["coverage_complete"], "report": str(args.report)}))
    return 0 if result["status"] == "reviewable" else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"P1 compare: {error}", file=sys.stderr)
        sys.exit(2)
