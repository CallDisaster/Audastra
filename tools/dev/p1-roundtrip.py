"""20轮打开→修改保存→独立重开验证框架；驱动为显式 argv JSON，不启动游戏。"""
from __future__ import annotations

import argparse
import copy
import importlib.util
import json
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode = True

_spec = importlib.util.spec_from_file_location("p1_support", Path(__file__).with_name("p1-support.py"))
support = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(support)


def validate_state(value):
    support.require(isinstance(value, dict) and value and all(isinstance(key, str) and key for key in value),
                    "语义 state 必须是非空稳定键对象")
    # state 由独立查询器提供，不能用源归档SHA/修订号代替语义字段。
    json.dumps(value, allow_nan=False)
    return value


def same_state(left, right):
    # JSON 的整数/布尔/数组类型不能被 Python 的 True == 1 隐式相等掩盖。
    return json.dumps(left, sort_keys=True, allow_nan=False) == json.dumps(right, sort_keys=True, allow_nan=False)


def render_argv(argv, values):
    support.require(isinstance(argv, list) and argv and all(isinstance(arg, str) and arg for arg in argv), "驱动必须为非空 argv 数组")
    rendered = []
    for arg in argv:
        for name, value in values.items():
            arg = arg.replace("{" + name + "}", str(value))
        support.require("{" not in arg and "}" not in arg, "命令中有未知占位符；参数 JSON 应写入独立文件")
        rendered.append(arg)
    return rendered


def run_roundtrip(source, output, plan, rounds=20, timeout=600, runner=None):
    support.require(plan.get("schema") == "p1-roundtrip-plan-v1", "驱动计划版本无效")
    support.require(plan.get("evidence_kind") in ("editor-cli", "editor-mcp", "synthetic-fixture"), "必须明确驱动证据类别")
    support.require(isinstance(plan.get("coverage"), list) and plan["coverage"] and
                    all(isinstance(item, str) and item for item in plan["coverage"]), "必须明确本次语义覆盖")
    for stage in ("open", "edit_save", "readback"):
        support.require(stage in plan, f"缺少 {stage} 驱动")
        render_argv(plan[stage], {key: "value" for key in ("input", "output", "source_sha256", "round")})
        required = ("input", "output") if stage == "edit_save" else (("input",) if stage == "open" else ("output",))
        support.require(all(any("{" + name + "}" in arg for arg in plan[stage]) for name in required),
                        f"{stage} 驱动必须绑定本轮地图路径")
    support.require(1 <= rounds <= 100, "轮数只允许1..100，阶段出口要求20")
    source, output = support.separate_paths(source, output)
    support.require(source.is_file() and not output.exists(), "来源须为文件，输出目录须不存在")
    runner = runner or (lambda argv: support.command_json(argv, timeout=timeout))
    source_sha = support.sha256(source)
    output.mkdir(parents=True)
    current = output / ("input" + source.suffix)
    with source.open("rb") as incoming, current.open("xb") as outgoing:
        shutil.copyfileobj(incoming, outgoing)
    support.require(support.sha256(current) == source_sha, "输入副本SHA不符")
    receipt = {"schema": "p1-continuous-roundtrip-v1", "status": "running", "evidence_kind": plan["evidence_kind"],
               "coverage": plan["coverage"], "requested_rounds": rounds, "completed_rounds": 0, "rounds": [],
               "source_sha256": source_sha, "runtime_validated": False, "original_editor_validated": False,
               "author_game_confirmation_required": True}
    expected = None
    try:
        for index in range(1, rounds + 1):
            target = output / (f"round-{index:02d}" + source.suffix)
            current_sha = support.sha256(current)
            values = {"input": current, "output": target, "source_sha256": current_sha, "round": index,
                      "expected_state_file": output / f"round-{index:02d}-expected.json"}
            expected_file = values.pop("expected_state_file")
            opened = runner(render_argv(plan["open"], values))
            support.require(opened.get("source_sha256") == current_sha, "打开回执没有绑定本轮源SHA")
            actual = validate_state(opened.get("state"))
            if expected is not None:
                support.require(same_state(actual, expected), "再次打开丢失或改变了前轮数据")
            else:
                expected = copy.deepcopy(actual)
            support.require(support.sha256(current) == current_sha and not target.exists(), "打开期间修改了源或提前写出候选")
            saved = runner(render_argv(plan["edit_save"], values))
            support.require(saved.get("input_sha256") == current_sha and target.is_file(), "保存回执身份/候选缺失")
            target_sha = support.sha256(target)
            support.require(saved.get("output_sha256") == target_sha, "保存回执输出SHA不符")
            support.require(target_sha != current_sha, "本轮没有实际修改")
            changes = saved.get("changes")
            validate_state(changes)
            support.require(any(key not in expected or not same_state(expected[key], value) for key, value in changes.items()), "changes 没有实际语义改动")
            # 所有前轮键默认保持，仅这次明确的修改可以覆盖；禁止悄悄删除前轮字段。
            next_expected = copy.deepcopy(expected)
            next_expected.update(copy.deepcopy(changes))
            support.write_json(expected_file, next_expected)
            # 重开驱动只获得候选路径及其实际SHA，不把预期state传给查询器。
            values["source_sha256"] = target_sha
            reopened = runner(render_argv(plan["readback"], values))
            support.require(reopened.get("source_sha256") == target_sha, "独立重开没有绑定保存候选SHA")
            support.require(same_state(validate_state(reopened.get("state")), next_expected), "独立重开回读与累计预期语义不同")
            support.require(support.sha256(current) == current_sha and support.sha256(target) == target_sha,
                            "驱动修改了当前源/候选")
            support.require(support.sha256(source) == source_sha, "只读原始来源变化")
            record = {"round": index, "input_sha256": current_sha, "output_sha256": target_sha, "changes": changes,
                      "open": opened, "save": saved, "readback": reopened, "cumulative_semantics_verified": True}
            support.write_json(output / f"round-{index:02d}-receipt.json", record)
            receipt["rounds"].append({"round": index, "receipt": f"round-{index:02d}-receipt.json",
                                      "input_sha256": current_sha, "output_sha256": target_sha})
            receipt["completed_rounds"] = index
            expected, current = next_expected, target
        receipt["status"] = "synthetic-framework-passed" if plan["evidence_kind"] == "synthetic-fixture" else "offline-roundtrip-passed"
        receipt["stage_d_twenty_round_gate_passed"] = rounds >= 20 and plan["evidence_kind"] != "synthetic-fixture"
        receipt["source_unchanged"] = support.sha256(source) == source_sha
        receipt["final_candidate"] = current.name
        return receipt
    except Exception as error:
        try:
            unchanged = support.sha256(source) == source_sha
        except OSError:
            unchanged = False
        receipt.update(status="failed", reason=str(error), source_unchanged=unchanged,
                       stage_d_twenty_round_gate_passed=False)
        raise
    finally:
        support.write_json(output / "roundtrip-report.json", receipt)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--plan", required=True, type=Path)
    parser.add_argument("--rounds", type=int, default=20)
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    support.require(args.timeout > 0, "timeout 必须为正数")
    result = run_roundtrip(args.source, args.output, support.read_json(args.plan), args.rounds, args.timeout)
    print(json.dumps({"status": result["status"], "completed_rounds": result["completed_rounds"],
                      "report": str(args.output / "roundtrip-report.json")}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"P1 roundtrip: {error}", file=sys.stderr)
        sys.exit(2)
